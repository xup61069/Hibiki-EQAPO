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
#include <array>
#include <cstdio>
#include <sstream>
#include <cmath>
#include <limits>
#include <QAction>
#include <QActionGroup>
#include <QAbstractScrollArea>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDockWidget>
#include <QDrag>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMap>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStringBuilder>
#include <QScrollArea>
#include <QFileInfo>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QSet>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolBar>
#include <QUuid>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "helpers/StringHelper.h"
#include "helpers/LogHelper.h"
#include "helpers/ChannelHelper.h"
#include "helpers/UiSnapshot.h"
#include "Editor/helpers/GUIChannelHelper.h"
#include "Editor/helpers/GUIHelper.h"
#include "version.h"
#include "FilterTable.h"
#include "MainWindow.h"
#include "StudioMotion.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#if defined(_M_AMD64)
#include "HibikiEQAPODriver/AsioProxyIdentity.h"
#include "HibikiEQAPODriver/AsioTargetStore.h"
#endif
#include "filters/loudnessCorrection/VolumeController.h"
#include "Editor/helpers/VolumeTakeoverManager.h"
#include "ui_MainWindow.h"
#ifdef EQAPO_ENABLE_UI_SNAPSHOTS
#include "guis/LoudnessCorrectionFilterGUI.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#endif

using namespace std;

static constexpr int WINDOW_LAYOUT_STATE_VERSION = 1;

static QString supportedLocaleName(const QLocale& locale)
{
	QString name = locale.name();
	if (name.startsWith("zh_"))
	{
		if (name.endsWith("_TW") || name.endsWith("_HK") || name.endsWith("_MO"))
			return "zh_TW";
		return "zh_CN";
	}
	if (locale.language() == QLocale::German)
		return "de";
	if (locale.language() == QLocale::French)
		return "fr";
	return "en";
}

static QString localeDisplayName(const QString& localeName)
{
	if (localeName == "zh_CN")
		return QString::fromUtf8("简体中文");
	if (localeName == "zh_TW")
		return QString::fromUtf8("繁體中文");
	if (localeName == "en")
		return "English";

	QString name = QLocale(localeName).nativeLanguageName();
	if (!name.isEmpty() && name[0].isLower())
		name[0] = name[0].toUpper();
	return name;
}

static void setStatusLevel(QLabel* label, const char* level)
{
	const QString statusLevel = QString::fromLatin1(level);
	if (label->property("statusLevel").toString() == statusLevel)
		return;

	label->setProperty("statusLevel", statusLevel);
	label->style()->unpolish(label);
	label->style()->polish(label);
	label->update();
}

class AnalysisStatusAccessible : public QAccessibleWidget
{
public:
	explicit AnalysisStatusAccessible(QWidget* widget)
		: QAccessibleWidget(widget, QAccessible::StaticText)
	{
	}

	QAccessible::State state() const override
	{
		QAccessible::State accessibleState = QAccessibleWidget::state();
		QObject* target = object();
		accessibleState.busy = target != NULL
			&& target->property("analysisBusy").toBool();
		return accessibleState;
	}
};

static QAccessibleInterface* createAnalysisStatusAccessible(
	const QString&, QObject* object)
{
	QLabel* label = qobject_cast<QLabel*>(object);
	if (label == NULL
		|| label->objectName() != QStringLiteral("analysisStateLabel"))
		return NULL;
	return new AnalysisStatusAccessible(label);
}

static void ensureAnalysisStatusAccessibility()
{
	static const bool installed = []() {
		QAccessible::installFactory(createAnalysisStatusAccessible);
		return true;
	}();
	Q_UNUSED(installed);
}

static void setAnalysisStatus(
	QLabel* label, const QString& text, const char* level, bool busy)
{
	if (label == NULL)
		return;

	const bool busyChanged = label->property("analysisBusy").toBool() != busy;
	label->setText(text);
	label->setAccessibleDescription(text);
	label->setProperty("analysisBusy", busy);
	setStatusLevel(label, level);

	if (busyChanged && QAccessible::isActive())
	{
		QAccessible::State changedState;
		changedState.busy = true;
		QAccessibleStateChangeEvent event(label, changedState);
		QAccessible::updateAccessibility(&event);
	}
}

static QByteArray serializeConfigurationLines(const QList<QString>& lines)
{
	QByteArray content;
	for (int index = 0; index < lines.size(); ++index)
	{
		if (index > 0)
			content.append("\r\n");
		content.append(lines[index].toUtf8());
	}
	return content;
}

static QList<QString> deserializeConfigurationLines(const QByteArray& content)
{
	QList<QString> lines;
	stringstream inputStream(string(content.constData(), content.size()));
	while (inputStream.good())
	{
		string encodedLine;
		getline(inputStream, encodedLine);
		if (!encodedLine.empty() && encodedLine.back() == '\r')
			encodedLine.pop_back();

		wstring line = StringHelper::toWString(encodedLine, CP_UTF8);
		if (line.find(L'\uFFFD') != wstring::npos)
			line = StringHelper::toWString(encodedLine, CP_ACP);
		lines.append(QString::fromStdWString(line));
	}
	return lines;
}

static QByteArray serializeConfigurationLinesLike(
	const QList<QString>& lines,
	const QByteArray& templateContent)
{
	const QByteArray lineEnding = templateContent.contains("\r\n")
		? QByteArray("\r\n")
		: (templateContent.contains('\n') ? QByteArray("\n") : QByteArray("\r\n"));
	const bool validUtf8 = templateContent.isEmpty()
		|| MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, templateContent.constData(),
			templateContent.size(), NULL, 0) > 0;
	const UINT codePage = validUtf8 ? CP_UTF8 : CP_ACP;

	QByteArray content;
	for (int index = 0; index < lines.size(); ++index)
	{
		if (index > 0)
			content.append(lineEnding);
		const string encoded = StringHelper::toString(
			lines[index].toStdWString(), codePage);
		content.append(encoded.data(), static_cast<int>(encoded.size()));
	}
	return content;
}

static QString configurationPathKey(const QString& path)
{
	QFileInfo pathInfo(path);
	QString pathKey = pathInfo.canonicalFilePath();
	if (pathKey.isEmpty())
		pathKey = pathInfo.absoluteFilePath();
	return QDir::cleanPath(pathKey).toCaseFolded();
}

class TemporaryProcessingMutexGuard
{
public:
	explicit TemporaryProcessingMutexGuard(DWORD timeoutMs = 5000)
	{
		handle = CreateMutexW(
			NULL, FALSE,
			L"Global\\EqualizerAPO.Editor.TemporaryProcessing.v1");
		if (handle == NULL)
			return;
		const DWORD waitResult = WaitForSingleObject(handle, timeoutMs);
		ownsMutex = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
	}

	~TemporaryProcessingMutexGuard()
	{
		if (ownsMutex)
			ReleaseMutex(handle);
		if (handle != NULL)
			CloseHandle(handle);
	}

	bool isLocked() const
	{
		return ownsMutex;
	}

private:
	HANDLE handle = NULL;
	bool ownsMutex = false;
};

enum class ConditionalWriteStatus
{
	written,
	conflict,
	failed
};

struct ConditionalWriteResult
{
	ConditionalWriteStatus status = ConditionalWriteStatus::failed;
	QByteArray currentContent;
	DWORD error = ERROR_SUCCESS;
	DWORD rollbackError = ERROR_SUCCESS;
	DWORD rollbackVerificationError = ERROR_SUCCESS;
	bool rollbackVerified = false;
};

static bool readConfigurationHandle(HANDLE file, QByteArray& content, DWORD& error)
{
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size))
	{
		error = GetLastError();
		return false;
	}
	if (size.QuadPart < 0 || size.QuadPart > std::numeric_limits<int>::max())
	{
		error = ERROR_FILE_TOO_LARGE;
		return false;
	}

	LARGE_INTEGER beginning = {};
	if (!SetFilePointerEx(file, beginning, NULL, FILE_BEGIN))
	{
		error = GetLastError();
		return false;
	}

	content.resize(static_cast<int>(size.QuadPart));
	DWORD totalRead = 0;
	while (totalRead < static_cast<DWORD>(content.size()))
	{
		DWORD bytesRead = 0;
		const DWORD remaining = static_cast<DWORD>(content.size()) - totalRead;
		if (!ReadFile(file, content.data() + totalRead, remaining, &bytesRead, NULL)
			|| bytesRead == 0)
		{
			error = GetLastError();
			if (error == ERROR_SUCCESS)
				error = ERROR_HANDLE_EOF;
			return false;
		}
		totalRead += bytesRead;
	}
	return true;
}

static bool writeConfigurationHandle(HANDLE file, const QByteArray& content, DWORD& error)
{
	LARGE_INTEGER beginning = {};
	if (!SetFilePointerEx(file, beginning, NULL, FILE_BEGIN))
	{
		error = GetLastError();
		return false;
	}

	DWORD totalWritten = 0;
	while (totalWritten < static_cast<DWORD>(content.size()))
	{
		DWORD bytesWritten = 0;
		const DWORD remaining = static_cast<DWORD>(content.size()) - totalWritten;
		if (!WriteFile(file, content.constData() + totalWritten, remaining,
			&bytesWritten, NULL) || bytesWritten == 0)
		{
			error = GetLastError();
			if (error == ERROR_SUCCESS)
				error = ERROR_WRITE_FAULT;
			return false;
		}
		totalWritten += bytesWritten;
	}
	if (!SetEndOfFile(file) || !FlushFileBuffers(file))
	{
		error = GetLastError();
		return false;
	}
	return true;
}

static ConditionalWriteResult conditionallyWriteConfiguration(
	const QString& path,
	const QByteArray& expectedContent,
	const QByteArray& replacementContent)
{
	ConditionalWriteResult result;
	HANDLE file = CreateFileW(
		path.toStdWString().c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		result.error = GetLastError();
		return result;
	}

	if (!readConfigurationHandle(file, result.currentContent, result.error))
	{
		CloseHandle(file);
		return result;
	}
	if (result.currentContent != expectedContent)
	{
		result.status = ConditionalWriteStatus::conflict;
		CloseHandle(file);
		return result;
	}

	DWORD writeError = ERROR_SUCCESS;
	if (!writeConfigurationHandle(file, replacementContent, writeError))
	{
		const QByteArray originalContent = result.currentContent;
		(void)writeConfigurationHandle(
			file, originalContent, result.rollbackError);
		QByteArray contentAfterRollback;
		if (readConfigurationHandle(
			file, contentAfterRollback, result.rollbackVerificationError))
		{
			result.currentContent = contentAfterRollback;
			result.rollbackVerified = contentAfterRollback == originalContent;
		}
		result.error = writeError;
		CloseHandle(file);
		return result;
	}

	result.status = ConditionalWriteStatus::written;
	CloseHandle(file);
	return result;
}

static bool rollbackConditionalConfigurationWrite(
	const QString& path,
	const QByteArray& temporaryContent,
	const QByteArray& originalContent)
{
	const ConditionalWriteResult rollback = conditionallyWriteConfiguration(
		path, temporaryContent, originalContent);
	return rollback.status == ConditionalWriteStatus::written ||
		(rollback.status == ConditionalWriteStatus::conflict &&
			rollback.currentContent == originalContent);
}

static quint64 processCreationToken(HANDLE process)
{
	FILETIME created;
	FILETIME exited;
	FILETIME kernel;
	FILETIME user;
	if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
		return 0;
	ULARGE_INTEGER value;
	value.LowPart = created.dwLowDateTime;
	value.HighPart = created.dwHighDateTime;
	return value.QuadPart;
}

enum class JournalOwnerStatus
{
	alive,
	dead,
	unknown
};

static JournalOwnerStatus journalOwnerStatus(
	quint32 processId,
	quint64 expectedCreationToken)
{
	if (processId == 0)
		return JournalOwnerStatus::dead;
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (process == NULL)
	{
		return GetLastError() == ERROR_INVALID_PARAMETER
			? JournalOwnerStatus::dead
			: JournalOwnerStatus::unknown;
	}
	DWORD exitCode = 0;
	if (!GetExitCodeProcess(process, &exitCode))
	{
		CloseHandle(process);
		return JournalOwnerStatus::unknown;
	}
	if (exitCode != STILL_ACTIVE)
	{
		CloseHandle(process);
		return JournalOwnerStatus::dead;
	}
	const quint64 actualCreationToken = processCreationToken(process);
	CloseHandle(process);
	if (expectedCreationToken != 0 && actualCreationToken == 0)
		return JournalOwnerStatus::unknown;
	if (expectedCreationToken != 0 && actualCreationToken != expectedCreationToken)
		return JournalOwnerStatus::dead;
	return JournalOwnerStatus::alive;
}

static bool hasConflictingTemporaryJournal(
	const QString& path,
	const QString& currentOwner)
{
	const QString requestedPathKey = configurationPathKey(path);
	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	settings.beginGroup(QStringLiteral("temporaryProcessing"));

	const QString legacyPath = settings.value(QStringLiteral("path")).toString();
	const int legacyVersion = settings.value(
		QStringLiteral("journalVersion")).toInt();
	const bool legacyCommitted = settings.value(
		QStringLiteral("committed"), legacyVersion < 2).toBool();
	if (legacyCommitted && !legacyPath.isEmpty()
		&& configurationPathKey(legacyPath) == requestedPathKey)
	{
		settings.endGroup();
		return true;
	}

	for (const QString& owner : settings.childGroups())
	{
		if (owner == currentOwner)
			continue;
		settings.beginGroup(owner);
		const int journalVersion = settings.value(
			QStringLiteral("journalVersion")).toInt();
		const bool committed = settings.value(
			QStringLiteral("committed"), journalVersion < 2).toBool();
		const QString journalPath = settings.value(QStringLiteral("path")).toString();
		settings.endGroup();
		if (committed && !journalPath.isEmpty()
			&& configurationPathKey(journalPath) == requestedPathKey)
		{
			settings.endGroup();
			return true;
		}
	}

	settings.endGroup();
	return false;
}

static QString analysisDeviceId(const shared_ptr<AbstractAPOInfo>& device)
{
	if (device == NULL)
		return QString();
	const wstring& guid = device->getDeviceGuid();
	return QString::fromStdWString(
		guid.empty() ? device->getDeviceString() : guid);
}

static double conservativePreampReduction(double peakGain)
{
	// Preamp controls display hundredths. Round the measured excess upward so
	// representation rounding cannot leave the sampled peak just above 0 dB.
	// Even a sub-hundredth positive excess needs one representable step.
	if (!std::isfinite(peakGain) || peakGain <= 0.0)
		return 0.0;
	double reduction = (std::max)(
		0.01, std::ceil(peakGain * 100.0) / 100.0);
	reduction = QString::number(reduction, 'f', 2).toDouble();
	// Multiplication can round nextafter(0.35, +inf) back to exactly 35 before
	// ceil() sees it. Enforce the safety postcondition on the serialized value.
	if (reduction < peakGain)
		reduction = QString::number(reduction + 0.01, 'f', 2).toDouble();
	return reduction >= peakGain ? reduction : 0.0;
}

static bool analysisFilesStillMatch(
	const QList<AnalysisConfigurationFileSnapshot>& snapshots)
{
	if (snapshots.isEmpty())
		return false;
	for (const AnalysisConfigurationFileSnapshot& snapshot : snapshots)
	{
		if (!snapshot.readable || snapshot.path.isEmpty())
			return false;
		QFile file(snapshot.path);
		if (!file.open(QIODevice::ReadOnly))
			return false;
		const QByteArray contents = file.readAll();
		const bool matched = file.error() == QFileDevice::NoError
			&& contents == snapshot.contents;
		file.close();
		if (!matched)
			return false;
	}
	return true;
}

static bool analysisRootMatchesEditor(
	const QList<AnalysisConfigurationFileSnapshot>& snapshots,
	const QString& rootPath,
	const QList<QString>& editorLines)
{
	if (rootPath.isEmpty())
		return false;
	const QString rootKey = configurationPathKey(rootPath);
	int matchingSnapshots = 0;
	for (const AnalysisConfigurationFileSnapshot& snapshot : snapshots)
	{
		if (configurationPathKey(snapshot.path) != rootKey)
			continue;
		++matchingSnapshots;
		if (!snapshot.readable
			|| deserializeConfigurationLines(snapshot.contents) != editorLines)
			return false;
	}
	// A recursive Include of the root is ambiguous, so require exactly the one
	// load that began this analysis.
	return matchingSnapshots == 1;
}

static bool analysisVolumesStillMatch(
	const QList<AnalysisVolumeSnapshot>& snapshots)
{
	for (const AnalysisVolumeSnapshot& snapshot : snapshots)
	{
		// A failed automatic-volume read made the analyzed filter fail closed.
		// Do not turn that temporary bypass into an actionable safety estimate.
		if (!snapshot.available)
			return false;
		VolumeController controller(snapshot.requestedEndpointId.toStdWString());
		EndpointVolumeState currentVolumeState;
		if (FAILED(controller.getVolumeState(currentVolumeState))
			|| QString::fromStdWString(controller.getEndpointId()).compare(
				snapshot.resolvedEndpointId, Qt::CaseInsensitive) != 0
			|| !std::isfinite(currentVolumeState.levelDb)
			|| !std::isfinite(currentVolumeState.scalar)
			|| currentVolumeState.levelDb != snapshot.volumeDb
			|| std::abs(currentVolumeState.scalar - snapshot.volumeScalar) > 1e-6
			|| currentVolumeState.muted != snapshot.muted)
			return false;
	}
	return true;
}

static bool analysisTopologySupportsAutoPreamp(
	const QList<AnalysisConfigurationFileSnapshot>& snapshots)
{
	// The regular response plot drives every input with the same impulse. That
	// is exact for independent per-channel LTI chains, but it is not a safe
	// arbitrary-input bound for cross-channel, time-varying, generated, staged,
	// conditional, dependency-backed, or opaque processing. An allowlist makes
	// new filter types fail closed until their analysis semantics are reviewed.
	static const QSet<QString> supportedCommands = {
		QStringLiteral("device"), QStringLiteral("include"),
		QStringLiteral("channel"), QStringLiteral("preamp"),
		QStringLiteral("parametriceq"), QStringLiteral("graphiceq"),
		QStringLiteral("delay"), QStringLiteral("loudnesscorrection"),
		QStringLiteral("vumeter"), QStringLiteral("headphonecalibration")
	};
	for (const AnalysisConfigurationFileSnapshot& snapshot : snapshots)
	{
		// Expressions can depend on state outside the configuration bytes and are
		// expanded before individual command parsers run. A fresh plot therefore
		// is not a stable bound for a later destructive edit.
		if (snapshot.contents.contains('`'))
			return false;
		for (const QString& line : deserializeConfigurationLines(snapshot.contents))
		{
			const QString trimmed = line.trimmed();
			if (trimmed.isEmpty() || trimmed.startsWith('#'))
				continue;
			const int separator = trimmed.indexOf(':');
			if (separator < 0)
				return false;
			const QString command = trimmed.left(separator).trimmed();
			const QString normalizedCommand = command.toCaseFolded();
			if (!supportedCommands.contains(normalizedCommand)
				&& !normalizedCommand.startsWith(QStringLiteral("filter")))
				return false;
		}
	}
	return true;
}

namespace
{
bool matchesStoredDevice(const shared_ptr<AbstractAPOInfo>& apoInfo, const QString& storedDevice)
{
	if (apoInfo == NULL || storedDevice.isEmpty())
		return false;

	const QString deviceString = QString::fromStdWString(apoInfo->getDeviceString());
	const QString deviceGuid = QString::fromStdWString(apoInfo->getDeviceGuid());
	return deviceString.compare(storedDevice, Qt::CaseInsensitive) == 0
		|| (!deviceGuid.isEmpty()
			&& (deviceGuid.compare(storedDevice, Qt::CaseInsensitive) == 0
				|| storedDevice.contains(deviceGuid, Qt::CaseInsensitive)));
}

bool hasInstalledDevice(const QList<shared_ptr<AbstractAPOInfo>>& devices)
{
	for (const shared_ptr<AbstractAPOInfo>& apoInfo : devices)
		if (apoInfo != NULL && apoInfo->isInstalled())
			return true;
	return false;
}
}

MainWindow::MainWindow(QDir configDir, QWidget* parent)
	: QMainWindow(parent), ui(new Ui::MainWindow), configDir(configDir)
{
	temporaryRecoveryOwner = QUuid::createUuid().toString(QUuid::WithoutBraces);
	temporaryRecoveryProcessStartedAt = processCreationToken(GetCurrentProcess());
	const bool snapshotMode = UiSnapshot::requested();
	noSavePreferences = snapshotMode;
	noSaveFilePreferences = snapshotMode;

	if (!snapshotMode)
	{
		outputDevices = toQList(DeviceAPOInfo::loadAllInfos(false));
		inputDevices = toQList(DeviceAPOInfo::loadAllInfos(true));
	}

	defaultOutputDevice = NULL;
	for (shared_ptr<AbstractAPOInfo>& apoInfo : outputDevices)
	{
		if (apoInfo->isDefaultDevice())
		{
			defaultOutputDevice = apoInfo;
			break;
		}
	}

	ui->setupUi(this);
	connect(ui->menuSettings, &QMenu::aboutToShow,
		this, &MainWindow::refreshWorkspaceActionState);
	doublePrecisionAction = ui->menuSettings->addAction(tr("Double precision (64-bit signal path)"));
	doublePrecisionAction->setObjectName(QStringLiteral("actionDoublePrecision"));
	doublePrecisionAction->setCheckable(true);
	doublePrecisionAction->setChecked(true);
	doublePrecisionAction->setToolTip(tr(
		"Applies to the current configuration. Off uses 32-bit audio between modules; specialized filters may retain double-precision internals. Save to apply when instant mode is off."));
	connect(doublePrecisionAction, &QAction::toggled, this, [this](bool enabled) {
		FilterTable* table = currentFilterTable();
		if (!table || !table->setDoublePrecision(enabled))
			showWorkspaceStatus(tr("Could not change precision. Check for duplicate or invalid ProcessingPrecision entries."), "warning");
		refreshWorkspaceActionState();
	});
	auto* deviceSelectorAction = ui->menuSettings->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Route), tr("Device Selector…"));
	deviceSelectorAction->setObjectName(QStringLiteral("actionDeviceSelector"));
	connect(deviceSelectorAction, &QAction::triggered, this, &MainWindow::runDeviceSelector);
	auto* scaleMenu = ui->menuView->addMenu(tr("Interface scale (restart)"));
	scaleMenu->setObjectName(QStringLiteral("interfaceScaleMenu"));
	auto* scaleGroup = new QActionGroup(scaleMenu);
	const int currentScale = qApp->property("studioInterfaceScale").toInt();
	for (int percent : {75, 90, 100, 110, 125, 150, 175, 200})
	{
		auto* action = scaleMenu->addAction(QStringLiteral("%1%").arg(percent));
		action->setCheckable(true);
		action->setData(percent);
		action->setChecked(percent == currentScale);
		scaleGroup->addAction(action);
		connect(action, &QAction::triggered, this, [this, percent, scaleGroup, currentScale]() {
			if (percent == currentScale) return;
			restart = true;
			if (close())
			{
				QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
				settings.setValue("interfaceScale", percent);
			}
			else
				for (QAction* choice : scaleGroup->actions())
					choice->setChecked(choice->data().toInt() == currentScale);
		});
	}
	auto* animationsAction = ui->menuView->addAction(tr("Interface animations"));
	animationsAction->setObjectName(QStringLiteral("actionInterfaceAnimations"));
	animationsAction->setCheckable(true);
	bool animationsEnabled = true;
	if (!snapshotMode)
	{
		QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
		animationsEnabled = settings.value("interfaceAnimations", true).toBool();
	}
	animationsAction->setChecked(animationsEnabled);
	qApp->setProperty("eqapoDisableAnimations", !animationsEnabled);
	connect(animationsAction, &QAction::toggled, this, [snapshotMode](bool enabled) {
		qApp->setProperty("eqapoDisableAnimations", !enabled);
		if (!snapshotMode)
		{
			QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
			settings.setValue("interfaceAnimations", enabled);
		}
	});
	QFrame* studioHeader = new QFrame(ui->centralWidget);
	studioHeader->setObjectName(QStringLiteral("studioHeader"));
	auto* headerLayout = new QHBoxLayout(studioHeader);
	headerLayout->setContentsMargins(10, 4, 10, 6);
	headerLayout->setSpacing(12);
	auto* identity = new QVBoxLayout;
	identity->setSpacing(0);
	auto* wordmark = new QLabel(QStringLiteral("HIBIKI"), studioHeader);
	wordmark->setObjectName(QStringLiteral("studioWordmark"));
	auto* edition = new QLabel(QStringLiteral("EQAPO / STUDIO"), studioHeader);
	edition->setObjectName(QStringLiteral("studioEdition"));
	identity->addWidget(wordmark);
	identity->addWidget(edition);
	headerLayout->addLayout(identity);
	auto* section = new QLabel(tr("Signal chain"), studioHeader);
	section->setObjectName(QStringLiteral("studioSection"));
	headerLayout->addWidget(section);
	headerLayout->addStretch();
	doublePrecisionCheckBox = new QCheckBox(tr("Double precision"), studioHeader);
	doublePrecisionCheckBox->setObjectName(QStringLiteral("doublePrecisionCheckBox"));
	doublePrecisionCheckBox->setChecked(true);
	doublePrecisionCheckBox->setToolTip(doublePrecisionAction->toolTip());
	doublePrecisionCheckBox->setAccessibleName(tr("Double precision signal path"));
	connect(doublePrecisionCheckBox, &QCheckBox::clicked,
		doublePrecisionAction, &QAction::setChecked);
	headerLayout->addWidget(doublePrecisionCheckBox);

	auto* asioDeviceLabel = new QLabel(tr("ASIO device:"), studioHeader);
	asioDeviceLabel->setProperty("toolbarRole", "context");
	headerLayout->addWidget(asioDeviceLabel);
	asioDeviceComboBox = new QComboBox(studioHeader);
	asioDeviceComboBox->setObjectName(QStringLiteral("asioDeviceComboBox"));
	asioDeviceComboBox->setMinimumWidth(GUIHelper::scale(180));
	asioDeviceComboBox->setMaximumWidth(GUIHelper::scale(300));
	asioDeviceComboBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	asioDeviceComboBox->setMinimumContentsLength(18);
	asioDeviceComboBox->setProperty("compactAudioControl", true);
	asioDeviceLabel->setBuddy(asioDeviceComboBox);
	QString asioAccessibleName = asioDeviceLabel->text();
	if (asioAccessibleName.endsWith(':') || asioAccessibleName.endsWith(QChar(0xFF1A)))
		asioAccessibleName.chop(1);
	asioDeviceComboBox->setAccessibleName(asioAccessibleName);
	connect(asioDeviceComboBox, SIGNAL(activated(int)),
		this, SLOT(asioDeviceSelected(int)));
	headerLayout->addWidget(asioDeviceComboBox);
	refreshAsioDeviceControl();
	ui->gridLayout->removeWidget(ui->tabWidget);
	ui->gridLayout->addWidget(studioHeader, 0, 0);
	ui->gridLayout->addWidget(ui->tabWidget, 1, 0);
	ui->gridLayout->setRowStretch(1, 1);
	QFrame* emptyState = new QFrame(ui->centralWidget);
	emptyState->setObjectName(QStringLiteral("studioEmptyState"));
	auto* emptyLayout = new QVBoxLayout(emptyState);
	emptyLayout->setContentsMargins(32, 20, 32, 20);
	emptyLayout->setSpacing(12);
	emptyLayout->addStretch();
	auto* emptyTitle = new QLabel(tr("Build your signal chain"), emptyState);
	emptyTitle->setObjectName(QStringLiteral("studioEmptyTitle"));
	emptyLayout->addWidget(emptyTitle);
	auto* emptyHint = new QLabel(tr("Open a configuration file or start with a new profile."), emptyState);
	emptyHint->setObjectName(QStringLiteral("studioEmptyHint"));
	emptyHint->setWordWrap(true);
	emptyLayout->addWidget(emptyHint);
	auto* emptyActions = new QHBoxLayout;
	for (QAction* action : {ui->actionNew, ui->actionOpen})
	{
		auto* button = new QPushButton(action->text(), emptyState);
		connect(button, &QPushButton::clicked, action, &QAction::trigger);
		emptyActions->addWidget(button);
	}
	emptyActions->addStretch();
	emptyLayout->addLayout(emptyActions);
	emptyLayout->addStretch();
	ui->gridLayout->addWidget(emptyState, 1, 0);
	connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this, emptyState](int) {
		emptyState->setVisible(ui->tabWidget->count() == 0);
		StudioMotion::reveal(ui->tabWidget->currentWidget());
	});
	// Keep every standard docking gesture available even when a saved layout
	// previously left the analysis panel floating.  These defaults are made
	// explicit because a QMainWindow state restore also restores the floating
	// state, which otherwise leaves no reliable in-app recovery command.
	setDockNestingEnabled(true);
	ui->analysisDockWidget->setAllowedAreas(Qt::AllDockWidgetAreas);
	ui->analysisDockWidget->setFeatures(
		QDockWidget::DockWidgetClosable
		| QDockWidget::DockWidgetMovable
		| QDockWidget::DockWidgetFloatable);
	dockAnalysisPanelAction = ui->menuView->addAction(tr("Dock analysis panel"));
	dockAnalysisPanelAction->setObjectName(QStringLiteral("actionDockAnalysisPanel"));
	dockAnalysisPanelAction->setToolTip(
		tr("Return the analysis panel to the bottom of the editor"));
	connect(
		dockAnalysisPanelAction,
		&QAction::triggered,
		this,
		&MainWindow::dockAnalysisPanel);
	const auto refreshDockRecoveryAction = [this]()
	{
		dockAnalysisPanelAction->setEnabled(
			ui->analysisDockWidget->isFloating()
			|| dockWidgetArea(ui->analysisDockWidget) == Qt::NoDockWidgetArea);
	};
	connect(
		ui->analysisDockWidget,
		&QDockWidget::topLevelChanged,
		this,
		[refreshDockRecoveryAction](bool) { refreshDockRecoveryAction(); });
	connect(
		ui->analysisDockWidget,
		&QDockWidget::dockLocationChanged,
		this,
		[refreshDockRecoveryAction](Qt::DockWidgetArea) { refreshDockRecoveryAction(); });
	refreshDockRecoveryAction();
	ui->startFromLabel->setBuddy(ui->startFromComboBox);
	ui->analysisChannelLabel->setBuddy(ui->analysisChannelComboBox);
	ui->resolutionLabel->setBuddy(ui->resolutionSpinBox);
	ui->startFromComboBox->setAccessibleName(ui->startFromLabel->text());
	ui->analysisChannelComboBox->setAccessibleName(ui->analysisChannelLabel->text());
	ui->resolutionSpinBox->setAccessibleName(ui->resolutionLabel->text());
	ui->actionResetAnalysisView->setIcon(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Restore));
	ui->resetAnalysisViewButton->setDefaultAction(ui->actionResetAnalysisView);
	ui->resetAnalysisViewButton->setAccessibleName(
		ui->actionResetAnalysisView->text());
	ui->resetAnalysisViewButton->setAccessibleDescription(
		ui->actionResetAnalysisView->toolTip());
	addAction(ui->actionResetAnalysisView);
	connect(
		ui->actionResetAnalysisView,
		SIGNAL(triggered()),
		ui->graphicsView,
		SLOT(resetView()));
	resize(GUIHelper::scale(snapshotMode ? QSize(1024, 768) : QSize(1280, 900)));
	ui->mainToolBar->setIconSize(GUIHelper::scale(QSize(19, 19)));
	ui->mainToolBar->setMovable(false);
	ui->mainToolBar->setFloatable(false);
	ui->tabWidget->setElideMode(Qt::ElideMiddle);
	ui->gridLayout->setContentsMargins(
		GUIHelper::scale(6),
		GUIHelper::scale(6),
		GUIHelper::scale(6),
		GUIHelper::scale(6));
	ui->gridLayout->setSpacing(GUIHelper::scale(5));
	ui->gridLayout_2->setContentsMargins(
		GUIHelper::scale(6),
		GUIHelper::scale(6),
		GUIHelper::scale(6),
		GUIHelper::scale(6));
	ui->gridLayout_2->setSpacing(GUIHelper::scale(5));
	ui->gridLayout_3->setContentsMargins(
		GUIHelper::scale(7),
		GUIHelper::scale(7),
		GUIHelper::scale(7),
		GUIHelper::scale(7));
	ui->gridLayout_3->setSpacing(GUIHelper::scale(4));
	ui->gridLayout_4->setContentsMargins(
		GUIHelper::scale(7),
		GUIHelper::scale(7),
		GUIHelper::scale(7),
		GUIHelper::scale(7));
	ui->gridLayout_4->setSpacing(GUIHelper::scale(4));

	LogHelper::set(stderr, true, false, false);

	QString version = QString("%1.%2").arg(MAJOR).arg(MINOR);
	if (REVISION != 0)
		version += QString(".%1").arg(REVISION);
	setWindowTitle(tr("Hibiki EQAPO %1 Configuration Editor").arg(version));

	QLabel* workspaceBrand = new QLabel(QStringLiteral("EQ"));
	workspaceBrand->setObjectName(QStringLiteral("workspaceBrand"));
	workspaceBrand->setToolTip(QStringLiteral("Hibiki EQAPO"));
	workspaceBrand->setAlignment(Qt::AlignCenter);
	ui->mainToolBar->insertWidget(ui->actionNew, workspaceBrand);
	ui->mainToolBar->insertSeparator(ui->actionNew);

	QWidget* spacer = new QWidget;
	spacer->setFixedWidth(GUIHelper::scale(8));
	ui->mainToolBar->addWidget(spacer);

	instantModeCheckBox = new QCheckBox(tr("Instant mode"));
	instantModeCheckBox->setObjectName(QStringLiteral("instantModeCheckBox"));
	instantModeCheckBox->setChecked(true);
	instantModeCheckBox->setToolTip(tr("Changes are saved immediately"));
	connect(instantModeCheckBox, SIGNAL(toggled(bool)), this, SLOT(instantModeEnabled(bool)));
	ui->mainToolBar->addWidget(instantModeCheckBox);

	spacer = new QWidget;
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	ui->mainToolBar->addWidget(spacer);

	QLabel* deviceLabel = new QLabel(tr("Device: "));
	deviceLabel->setProperty("toolbarRole", "context");
	ui->mainToolBar->addWidget(deviceLabel);

	deviceComboBox = new QComboBox;
	deviceComboBox->setObjectName(QStringLiteral("deviceComboBox"));
	deviceLabel->setBuddy(deviceComboBox);
	deviceComboBox->setAccessibleName(deviceLabel->text().trimmed());
	connect(deviceComboBox, SIGNAL(activated(int)), this, SLOT(deviceSelected(int)));
	ui->mainToolBar->addWidget(deviceComboBox);

	spacer = new QWidget;
	spacer->setFixedWidth(GUIHelper::scale(8));
	ui->mainToolBar->addWidget(spacer);

	QLabel* channelConfigurationLabel = new QLabel(tr("Channel configuration: "));
	channelConfigurationLabel->setProperty("toolbarRole", "context");
	ui->mainToolBar->addWidget(channelConfigurationLabel);

	channelConfigurationComboBox = new QComboBox;
	channelConfigurationComboBox->setObjectName(
		QStringLiteral("channelConfigurationComboBox"));
	channelConfigurationComboBox->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	channelConfigurationLabel->setBuddy(channelConfigurationComboBox);
	channelConfigurationComboBox->setAccessibleName(
		channelConfigurationLabel->text().trimmed());
	ui->mainToolBar->addWidget(channelConfigurationComboBox);

	if (snapshotMode)
	{
		deviceComboBox->addItem(QStringLiteral("No audio device"));
		deviceComboBox->setEnabled(false);
	}
	else
	{
		QStandardItemModel* model = qobject_cast<QStandardItemModel*>(deviceComboBox->model());
		if (defaultOutputDevice != NULL)
			deviceComboBox->addItem(tr("Default") + " (" + QString::fromStdWString(defaultOutputDevice->getConnectionName()) + " - " + QString::fromStdWString(defaultOutputDevice->getDeviceName()) + ")", NULL);

		deviceComboBox->addItem(tr("Playback devices:"));
		QStandardItem* item = model->item(model->rowCount() - 1);
		QFont font = item->font();
		font.setBold(true);
		item->setFont(font);
		item->setSelectable(false);

		for (shared_ptr<AbstractAPOInfo>& apoInfo : outputDevices)
			if (apoInfo->isInstalled())
				deviceComboBox->addItem(QString::fromStdWString(apoInfo->getConnectionName()) + " - " + QString::fromStdWString(apoInfo->getDeviceName()), QVariant::fromValue(apoInfo));

		deviceComboBox->addItem(tr("Capture devices:"));
		item = model->item(model->rowCount() - 1);
		item->setFont(font);
		item->setSelectable(false);

		for (shared_ptr<AbstractAPOInfo>& apoInfo : inputDevices)
			if (apoInfo->isInstalled())
				deviceComboBox->addItem(QString::fromStdWString(apoInfo->getConnectionName()) + " - " + QString::fromStdWString(apoInfo->getDeviceName()), QVariant::fromValue(apoInfo));
	}

	connect(channelConfigurationComboBox, SIGNAL(activated(int)), this, SLOT(channelConfigurationSelected(int)));

	setupWorkspaceTools();
	if (!snapshotMode)
		recoverInterruptedTemporaryProcessingState();

	analysisPlotScene = new AnalysisPlotScene(ui->graphicsView);
	ui->graphicsView->setScene(analysisPlotScene);

	QLabel* headroomLabel = new QLabel(tr("Headroom:"), ui->groupBox_2);
	headroomValueLabel = new QLabel(QStringLiteral("—"), ui->groupBox_2);
	headroomValueLabel->setObjectName(QStringLiteral("headroomValueLabel"));
	headroomValueLabel->setAccessibleName(tr("Estimated headroom"));
	ensureAnalysisStatusAccessibility();
	analysisStateLabel = new QLabel(tr("Waiting for analysis"), ui->groupBox_2);
	analysisStateLabel->setObjectName(QStringLiteral("analysisStateLabel"));
	analysisStateLabel->setWordWrap(true);
	analysisStateLabel->setAccessibleName(tr("Analysis status"));
	setAnalysisStatus(
		analysisStateLabel, analysisStateLabel->text(), "normal", false);
	autoPreampButton = new QPushButton(
		tr("Auto preamp (current ≤ 0 dB)"), ui->groupBox_2);
	autoPreampButton->setObjectName(QStringLiteral("autoPreampButton"));
	autoPreampButton->setToolTip(tr(
		"One-time estimated cut for a supported static per-channel chain from the latest saved current-file analysis; review and save manually"));
	autoPreampButton->setEnabled(false);
	connect(
		autoPreampButton,
		&QPushButton::clicked,
		this,
		&MainWindow::lowerPreampToPreventClipping);
	ui->gridLayout_4->addWidget(headroomLabel, 4, 0);
	ui->gridLayout_4->addWidget(headroomValueLabel, 4, 1);
	ui->gridLayout_4->addWidget(analysisStateLabel, 5, 0, 1, 2);
	ui->gridLayout_4->addWidget(autoPreampButton, 6, 0, 1, 2);
	// A horizontal analysis setup strip leaves room for the signal chain on
	// compact displays, while keeping measurements alongside the response plot.
	const QList<QWidget*> analysisControls = {
		ui->startFromLabel, ui->startFromComboBox,
		ui->analysisChannelLabel, ui->analysisChannelComboBox,
		ui->resolutionLabel, ui->resolutionSpinBox, ui->resetAnalysisViewButton
	};
	for (QWidget* control : analysisControls)
		ui->gridLayout_3->removeWidget(control);
	for (int column = 0; column < analysisControls.size(); ++column)
		ui->gridLayout_3->addWidget(analysisControls[column], 0, column);
	ui->gridLayout_3->setColumnStretch(1, 1);
	ui->gridLayout_3->setColumnStretch(3, 1);
	ui->gridLayout_2->removeWidget(ui->groupBox);
	ui->gridLayout_2->removeWidget(ui->groupBox_2);
	ui->gridLayout_2->removeWidget(ui->graphicsView);
	ui->gridLayout_2->addWidget(ui->groupBox, 0, 0, 1, 2);
	ui->gridLayout_2->addWidget(ui->groupBox_2, 1, 0);
	ui->gridLayout_2->setAlignment(ui->groupBox_2, Qt::AlignTop);
	ui->gridLayout_2->addWidget(ui->graphicsView, 1, 1);
	ui->gridLayout_2->setRowStretch(0, 0);
	ui->gridLayout_2->setRowStretch(1, 1);

	analysisThread = new AnalysisThread;
	analysisThread->start();
	connect(analysisThread, SIGNAL(analysisFinished()), this, SLOT(updateAnalysisPanel()));

	QString automaticLocale = supportedLocaleName(QLocale::system());
	const char* localeNames[] = {"", "en", "de", "fr", "zh_CN", "zh_TW"};
	for (size_t i = 0; i < sizeof(localeNames) / sizeof(localeNames[0]); ++i)
	{
		QString localeName = QString::fromLatin1(localeNames[i]);
		QString text = localeName.isEmpty()
			? tr("Automatic (%1)").arg(localeDisplayName(automaticLocale))
			: localeDisplayName(localeName);
		QAction* action = ui->menuLanguage->addAction(text);
		action->setData(localeName);
		action->setCheckable(true);
		connect(action, SIGNAL(triggered(bool)), this, SLOT(languageSelected(bool)));
	}

	if (!snapshotMode)
	{
		loadPreferences();
		setupTrayIcon();
	}
	refreshProfiles();
	syncProfileSelection();
	refreshWorkspaceActionState();
	bool useInitialStudioLayout = snapshotMode;
	if (!snapshotMode)
	{
		QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
		useInitialStudioLayout = !settings.contains("windowState");
	}
	if (useInitialStudioLayout)
		QTimer::singleShot(0, this, [this]() {
			resizeDocks({ui->analysisDockWidget}, {GUIHelper::scale(250)}, Qt::Vertical);
			QTimer::singleShot(0, ui->graphicsView, &FrequencyPlotView::resetView);
		});
}

MainWindow::~MainWindow()
{
	disconnect(analysisThread, NULL, this, NULL);
	delete analysisThread;
	delete ui;
}

void MainWindow::setupWorkspaceTools()
{
	ui->actionNew->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::NewDocument));
	ui->actionOpen->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::OpenFolder));
	ui->actionSave->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Save));
	ui->actionSaveAs->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::SaveAs));
	ui->actionCut->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Cut));
	ui->actionCopy->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Copy));
	ui->actionPaste->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Paste));
	ui->actionDelete->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Delete));
	ui->actionSelectAll->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::SelectAll));

	workspaceToolBar = new QToolBar(tr("Workspace tools"), this);
	workspaceToolBar->setObjectName(QStringLiteral("workspaceToolBar"));
	workspaceToolBar->setMovable(false);
	workspaceToolBar->setFloatable(false);
	workspaceToolBar->setIconSize(GUIHelper::scale(QSize(18, 18)));
	addToolBarBreak(Qt::TopToolBarArea);
	addToolBar(Qt::TopToolBarArea, workspaceToolBar);

	QLabel* profileLabel = new QLabel(tr("Profile:"), workspaceToolBar);
	profileLabel->setProperty("toolbarRole", "context");
	workspaceToolBar->addWidget(profileLabel);

	profileComboBox = new QComboBox(workspaceToolBar);
	profileComboBox->setObjectName(QStringLiteral("profileComboBox"));
	profileComboBox->setMinimumContentsLength(18);
	profileComboBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	profileComboBox->setAccessibleName(tr("Configuration profile"));
	profileComboBox->setToolTip(tr("Open a configuration profile from the Hibiki EQAPO config folder"));
	connect(profileComboBox, SIGNAL(activated(int)), this, SLOT(profileSelected(int)));
	workspaceToolBar->addWidget(profileComboBox);

	QAction* duplicateAction = workspaceToolBar->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Duplicate),
		tr("Duplicate profile"));
	duplicateAction->setToolTip(tr("Duplicate the current profile"));
	connect(duplicateAction, SIGNAL(triggered()), this, SLOT(duplicateCurrentProfile()));

	workspaceToolBar->addSeparator();
	searchLineEdit = new QLineEdit(workspaceToolBar);
	searchLineEdit->setObjectName(QStringLiteral("filterSearchLineEdit"));
	searchLineEdit->setPlaceholderText(tr("Find a filter or setting…"));
	searchLineEdit->setClearButtonEnabled(true);
	searchLineEdit->setMinimumWidth(GUIHelper::scale(190));
	searchLineEdit->setMaximumWidth(GUIHelper::scale(310));
	searchLineEdit->setAccessibleName(tr("Find a filter or setting"));
	connect(searchLineEdit, SIGNAL(returnPressed()), this, SLOT(findNext()));
	connect(searchLineEdit, SIGNAL(textChanged(QString)), this, SLOT(searchTextChanged(QString)));
	workspaceToolBar->addWidget(searchLineEdit);

	QAction* findNextAction = workspaceToolBar->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::FindNext),
		tr("Find next"));
	findNextAction->setShortcut(QKeySequence::FindNext);
	findNextAction->setToolTip(tr("Find next (F3)"));
	connect(findNextAction, SIGNAL(triggered()), this, SLOT(findNext()));

	QAction* findPreviousAction = new QAction(tr("Find previous"), this);
	findPreviousAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
	connect(findPreviousAction, SIGNAL(triggered()), this, SLOT(findPrevious()));
	addAction(findPreviousAction);

	QAction* focusSearchAction = new QAction(tr("Find"), this);
	focusSearchAction->setShortcut(QKeySequence::Find);
	connect(focusSearchAction, &QAction::triggered, this, [this]() {
		searchLineEdit->setFocus(Qt::ShortcutFocusReason);
		searchLineEdit->selectAll();
	});
	addAction(focusSearchAction);

	QAction* clearSearchAction = new QAction(searchLineEdit);
	clearSearchAction->setShortcut(Qt::Key_Escape);
	clearSearchAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(clearSearchAction, &QAction::triggered, this, [this]() {
		searchLineEdit->clear();
		if (FilterTable* filterTable = currentFilterTable())
			filterTable->clearFindSelection();
	});
	searchLineEdit->addAction(clearSearchAction);

	workspaceToolBar->addSeparator();
	captureComparisonAction = workspaceToolBar->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Snapshot),
		tr("Capture A"));
	captureComparisonAction->setToolTip(tr("Capture the current filter state as comparison A"));
	connect(captureComparisonAction, SIGNAL(triggered()), this, SLOT(captureComparisonA()));

	comparisonAction = workspaceToolBar->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Compare),
		tr("Compare A"));
	comparisonAction->setCheckable(true);
	comparisonAction->setEnabled(false);
	comparisonAction->setToolTip(tr("Switch between captured A and the current B state"));
	connect(comparisonAction, SIGNAL(toggled(bool)), this, SLOT(comparisonToggled(bool)));

	bypassAction = workspaceToolBar->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Bypass),
		tr("Bypass"));
	bypassAction->setCheckable(true);
	bypassAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
	bypassAction->setToolTip(tr("Temporarily bypass every command in the current profile"));
	connect(bypassAction, SIGNAL(toggled(bool)), this, SLOT(bypassToggled(bool)));

	QWidget* statusSpacer = new QWidget(workspaceToolBar);
	statusSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	workspaceToolBar->addWidget(statusSpacer);
	workspaceStatusLabel = new QLabel(tr("Ready"), workspaceToolBar);
	workspaceStatusLabel->setObjectName(QStringLiteral("workspaceStatusLabel"));
	workspaceStatusLabel->setProperty("workspaceStatus", true);
	workspaceStatusLabel->setProperty("statusLevel", "normal");
	workspaceStatusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	workspaceStatusLabel->setAccessibleName(tr("Workspace status"));
	workspaceStatusLabel->setAccessibleDescription(workspaceStatusLabel->text());
	workspaceToolBar->addWidget(workspaceStatusLabel);

	profileMenu = new QMenu(tr("Profiles"), ui->menuBar);
	profileMenu->setObjectName(QStringLiteral("profileMenu"));
	openProfilesMenu = profileMenu->addMenu(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Profile),
		tr("Open profile"));
	profileMenu->addSeparator();
	QAction* menuDuplicateAction = profileMenu->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Duplicate),
		tr("Duplicate current profile…"));
	connect(menuDuplicateAction, SIGNAL(triggered()), this, SLOT(duplicateCurrentProfile()));
	QAction* renameAction = profileMenu->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Rename),
		tr("Rename current profile…"));
	connect(renameAction, SIGNAL(triggered()), this, SLOT(renameCurrentProfile()));
	profileMenu->addSeparator();
	QAction* importAction = profileMenu->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Import),
		tr("Import profile…"));
	connect(importAction, SIGNAL(triggered()), this, SLOT(importProfile()));
	QAction* exportAction = profileMenu->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Export),
		tr("Export current profile…"));
	connect(exportAction, SIGNAL(triggered()), this, SLOT(exportCurrentProfile()));
	profileMenu->addSeparator();
	QAction* linkAction = profileMenu->addAction(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Link),
		tr("Link current profile to selected device"));
	connect(linkAction, SIGNAL(triggered()), this, SLOT(linkCurrentProfileToDevice()));
	QAction* clearLinkAction = profileMenu->addAction(tr("Clear selected device profile link"));
	connect(clearLinkAction, SIGNAL(triggered()), this, SLOT(clearCurrentDeviceProfileLink()));
	ui->menuBar->insertMenu(ui->menuView->menuAction(), profileMenu);

	closeToTrayAction = ui->menuSettings->addAction(tr("Keep running in the notification area"));
	closeToTrayAction->setCheckable(true);
	connect(closeToTrayAction, SIGNAL(toggled(bool)), this, SLOT(closeToTrayToggled(bool)));

	takeoverVolumeKeysAction = ui->menuSettings->addAction(tr("Take over Windows volume keys & OSD"));
	takeoverVolumeKeysAction->setCheckable(true);
	connect(takeoverVolumeKeysAction, &QAction::toggled, this, &MainWindow::takeoverVolumeKeysToggled);

	profileWatcher = new QFileSystemWatcher(this);
	if (configDir.exists())
		profileWatcher->addPath(configDir.absolutePath());
	connect(profileWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) {
		refreshProfiles();
	});
}

void MainWindow::refreshAsioDeviceControl()
{
	if (asioDeviceComboBox == NULL)
		return;
	QSignalBlocker blocker(asioDeviceComboBox);
	asioDeviceComboBox->clear();
	const QString guidance = tr(
		"Chooses the hardware driver behind Hibiki EQAPO. Reopen the DAW audio device after changing it.");
	asioDeviceComboBox->setToolTip(guidance);
	if (UiSnapshot::requested())
	{
		asioDeviceComboBox->addItem(tr("ASIO device unavailable"));
		asioDeviceComboBox->setEnabled(false);
		return;
	}

#if defined(_M_AMD64)
	const QString proxyPath = QDir(QCoreApplication::applicationDirPath()).filePath(
		QString::fromWCharArray(HibikiAsio::kDriverDllName));
	const HibikiAsio::AsioTargetControlState state =
		HibikiAsio::inspectAsioTargets(proxyPath.toStdWString());
	if (!state.proxyInstalled)
	{
		asioDeviceComboBox->addItem(tr("ASIO proxy not installed"));
		asioDeviceComboBox->setEnabled(false);
		return;
	}
	if (state.candidates.empty())
	{
		asioDeviceComboBox->addItem(tr("No compatible x64 ASIO driver"));
		asioDeviceComboBox->setEnabled(false);
		return;
	}

	auto candidateName = [&state](const CLSID& clsid) -> QString {
		for (const HibikiAsio::AsioDriverCandidate& candidate : state.candidates)
			if (InlineIsEqualGUID(candidate.clsid, clsid) != FALSE)
				return QString::fromStdWString(candidate.name);
		return {};
	};
	QString automaticLabel = tr("Automatic / installer default");
	if (state.machineDefault.present && state.machineDefault.valid)
	{
		const QString name = candidateName(state.machineDefault.clsid);
		if (!name.isEmpty())
			automaticLabel += QStringLiteral(" — ") + name;
		else
			automaticLabel += QStringLiteral(" — ") + tr("unavailable");
	}
	else if (!state.machineDefault.present && state.candidates.size() == 1)
	{
		automaticLabel += QStringLiteral(" — ") +
			QString::fromStdWString(state.candidates.front().name);
	}
	else
	{
		automaticLabel += QStringLiteral(" — ") + tr("unavailable");
	}
	asioDeviceComboBox->addItem(automaticLabel, QStringLiteral("default"));

	int selectedIndex = state.userOverride.present ? -1 : 0;
	for (const HibikiAsio::AsioDriverCandidate& candidate : state.candidates)
	{
		std::array<wchar_t, 40> clsidText{};
		if (StringFromGUID2(candidate.clsid, clsidText.data(),
			static_cast<int>(clsidText.size())) == 0)
		{
			continue;
		}
		const int index = asioDeviceComboBox->count();
		asioDeviceComboBox->addItem(
			QString::fromStdWString(candidate.name),
			QString::fromWCharArray(clsidText.data()));
		asioDeviceComboBox->setItemData(
			index, QString::fromStdWString(candidate.dllPath), Qt::ToolTipRole);
		if (state.userOverride.present && state.userOverride.valid &&
			InlineIsEqualGUID(candidate.clsid, state.userOverride.clsid) != FALSE)
		{
			selectedIndex = index;
		}
	}
	if (selectedIndex < 0)
	{
		asioDeviceComboBox->insertItem(
			0, tr("Current ASIO device unavailable"), QStringLiteral("unavailable"));
		selectedIndex = 0;
	}
	asioDeviceComboBox->setCurrentIndex(selectedIndex);
	asioDeviceComboBox->setEnabled(true);
#else
	asioDeviceComboBox->addItem(tr("Available in the x64 editor only"));
	asioDeviceComboBox->setEnabled(false);
#endif
}

void MainWindow::asioDeviceSelected(int index)
{
	if (asioDeviceComboBox == NULL || index < 0)
		return;
#if defined(_M_AMD64)
	const QString selection = asioDeviceComboBox->itemData(index).toString();
	if (selection == QStringLiteral("unavailable") || selection.isEmpty())
	{
		refreshAsioDeviceControl();
		return;
	}
	const QString proxyPath = QDir(QCoreApplication::applicationDirPath()).filePath(
		QString::fromWCharArray(HibikiAsio::kDriverDllName));
	LONG win32Error = ERROR_SUCCESS;
	HibikiAsio::AsioTargetUpdateStatus result;
	if (selection == QStringLiteral("default"))
	{
		result = HibikiAsio::clearUserAsioTarget(
			proxyPath.toStdWString(), &win32Error);
	}
	else
	{
		CLSID clsid{};
		if (CLSIDFromString(selection.toStdWString().c_str(), &clsid) != S_OK)
		{
			refreshAsioDeviceControl();
			return;
		}
		result = HibikiAsio::setUserAsioTarget(
			proxyPath.toStdWString(), clsid, &win32Error);
	}
	if (result == HibikiAsio::AsioTargetUpdateStatus::Updated)
	{
		showWorkspaceStatus(tr("ASIO device saved. Reopen the DAW audio device to apply."));
	}
	else
	{
		showWorkspaceStatus(
			win32Error == ERROR_SUCCESS
				? tr("Could not change the ASIO device.")
				: tr("Could not change the ASIO device (Windows error %1).")
					.arg(win32Error),
			"warning");
	}
	refreshAsioDeviceControl();
#else
	Q_UNUSED(index);
#endif
}

bool MainWindow::restoreWindowLayoutState(const QByteArray& state)
{
	return restoreState(state, WINDOW_LAYOUT_STATE_VERSION);
}

QByteArray MainWindow::saveWindowLayoutState() const
{
	return saveState(WINDOW_LAYOUT_STATE_VERSION);
}

void MainWindow::setupTrayIcon()
{
	if (!QSystemTrayIcon::isSystemTrayAvailable())
	{
		closeToTray = false;
		if (closeToTrayAction != NULL)
		{
			QSignalBlocker blocker(closeToTrayAction);
			closeToTrayAction->setChecked(false);
			closeToTrayAction->setEnabled(false);
			closeToTrayAction->setToolTip(tr("The Windows notification area is unavailable"));
		}
		return;
	}

	trayIcon = new QSystemTrayIcon(this);
	trayIcon->setObjectName(QStringLiteral("workspaceTrayIcon"));
	trayIcon->setIcon(windowIcon().isNull()
		? GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Profile, true)
		: windowIcon());
	trayIcon->setToolTip(tr("Hibiki EQAPO Configuration Editor"));

	QMenu* trayMenu = new QMenu(this);
	QAction* showAction = trayMenu->addAction(tr("Show Configuration Editor"));
	connect(showAction, &QAction::triggered, this, [this]() {
		showNormal();
		raise();
		activateWindow();
	});
	trayProfileMenu = trayMenu->addMenu(
		GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Profile),
		tr("Profiles"));
	trayMenu->addSeparator();
	QAction* trayInstantAction = trayMenu->addAction(tr("Instant mode"));
	trayInstantAction->setCheckable(true);
	trayInstantAction->setChecked(instantModeCheckBox->isChecked());
	connect(trayInstantAction, &QAction::toggled, instantModeCheckBox, &QCheckBox::setChecked);
	connect(instantModeCheckBox, &QCheckBox::toggled, this, [trayInstantAction](bool checked) {
		QSignalBlocker blocker(trayInstantAction);
		trayInstantAction->setChecked(checked);
	});
	trayMenu->addAction(bypassAction);
	trayMenu->addAction(closeToTrayAction);
	trayMenu->addAction(takeoverVolumeKeysAction);
	trayMenu->addSeparator();
	QAction* quitAction = trayMenu->addAction(tr("Exit"));
	connect(quitAction, &QAction::triggered, this, [this]() {
		quitRequested = true;
		if (!restoreTemporaryProcessingState())
		{
			quitRequested = false;
			return;
		}
		show();
		close();
	});

	trayIcon->setContextMenu(trayMenu);
	connect(trayIcon, &QSystemTrayIcon::activated, this,
		[this](QSystemTrayIcon::ActivationReason reason) {
			if (reason != QSystemTrayIcon::Trigger && reason != QSystemTrayIcon::DoubleClick)
				return;
			if (isVisible())
				hide();
			else
			{
				showNormal();
				raise();
				activateWindow();
			}
		});
	trayIcon->show();
	refreshProfileMenus();
}

FilterTable* MainWindow::currentFilterTable() const
{
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	return scrollArea == NULL ? NULL : qobject_cast<FilterTable*>(scrollArea->widget());
}

QString MainWindow::currentProfilePath() const
{
	FilterTable* filterTable = currentFilterTable();
	return filterTable == NULL ? QString() : QDir::cleanPath(filterTable->getConfigPath());
}

bool MainWindow::isFilterTableDirty(const FilterTable* filterTable) const
{
	if (filterTable == NULL)
		return false;
	for (int index = 0; index < ui->tabWidget->count(); ++index)
	{
		QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(index));
		if (scrollArea != NULL && scrollArea->widget() == filterTable)
			return ui->tabWidget->tabText(index).endsWith('*');
	}
	return false;
}

void MainWindow::showWorkspaceStatus(const QString& text, const char* level, int timeoutMs)
{
	if (workspaceStatusLabel == NULL)
		return;

	workspaceStatusRevision++;
	const quint64 revision = workspaceStatusRevision;
	workspaceStatusLabel->setText(text);
	StudioMotion::feedback(workspaceStatusLabel);
	workspaceStatusLabel->setAccessibleDescription(text);
	setStatusLevel(workspaceStatusLabel, level);
	workspaceStatusLabel->setToolTip(text);
	if (trayIcon != NULL)
		trayIcon->setToolTip(tr("Hibiki EQAPO · %1").arg(text));

	if (timeoutMs > 0)
	{
		QTimer::singleShot(timeoutMs, this, [this, revision]() {
			if (workspaceStatusRevision != revision || workspaceStatusLabel == NULL)
				return;
			workspaceStatusLabel->setText(tr("Ready"));
			workspaceStatusLabel->setAccessibleDescription(workspaceStatusLabel->text());
			setStatusLevel(workspaceStatusLabel, "normal");
		});
	}
}

void MainWindow::refreshProfiles()
{
	if (profileComboBox == NULL)
		return;

	const QString activePath = currentProfilePath();
	const QSignalBlocker blocker(profileComboBox);
	profileComboBox->clear();
	const QFileInfoList profiles = configDir.entryInfoList(
		QStringList() << QStringLiteral("*.txt"),
		QDir::Files | QDir::Readable | QDir::NoSymLinks,
		QDir::Name | QDir::IgnoreCase);
	for (const QFileInfo& profile : profiles)
	{
		profileComboBox->addItem(
			GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Profile),
			profile.completeBaseName(),
			QDir::toNativeSeparators(profile.absoluteFilePath()));
	}
	profileComboBox->setEnabled(!profiles.isEmpty());
	profileComboBox->setToolTip(profiles.isEmpty()
		? tr("No configuration profiles were found in %1").arg(configDir.absolutePath())
		: tr("Open a configuration profile from %1").arg(configDir.absolutePath()));

	for (int index = 0; index < profileComboBox->count(); ++index)
	{
		if (QDir::cleanPath(profileComboBox->itemData(index).toString()).compare(
			activePath, Qt::CaseInsensitive) == 0)
		{
			profileComboBox->setCurrentIndex(index);
			break;
		}
	}
	refreshProfileMenus();
}

void MainWindow::refreshProfileMenus()
{
	if (openProfilesMenu != NULL)
		openProfilesMenu->clear();
	if (trayProfileMenu != NULL)
		trayProfileMenu->clear();

	const QString activePath = currentProfilePath();
	for (int index = 0; profileComboBox != NULL && index < profileComboBox->count(); ++index)
	{
		const QString name = profileComboBox->itemText(index);
		const QString path = profileComboBox->itemData(index).toString();
		auto addProfileAction = [this, &name, &path, &activePath](QMenu* menu) {
			if (menu == NULL)
				return;
			QAction* action = menu->addAction(
				GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Profile), name);
			action->setCheckable(true);
			action->setChecked(QDir::cleanPath(path).compare(activePath, Qt::CaseInsensitive) == 0);
			connect(action, &QAction::triggered, this, [this, path]() {
				if (!restoreTemporaryProcessingState())
					return;
				const bool opened = load(path);
				showNormal();
				raise();
				activateWindow();
				syncProfileSelection();
				if (opened)
					showWorkspaceStatus(tr("Opened %1").arg(QFileInfo(path).completeBaseName()));
			});
		};
		addProfileAction(openProfilesMenu);
		addProfileAction(trayProfileMenu);
	}

	if (openProfilesMenu != NULL && openProfilesMenu->isEmpty())
		openProfilesMenu->addAction(tr("No profiles found"))->setEnabled(false);
	if (trayProfileMenu != NULL && trayProfileMenu->isEmpty())
		trayProfileMenu->addAction(tr("No profiles found"))->setEnabled(false);
}

void MainWindow::syncProfileSelection()
{
	if (profileComboBox == NULL)
		return;

	const QString path = currentProfilePath();
	const QSignalBlocker blocker(profileComboBox);
	int selectedIndex = -1;
	for (int index = 0; index < profileComboBox->count(); ++index)
	{
		if (QDir::cleanPath(profileComboBox->itemData(index).toString()).compare(
			path, Qt::CaseInsensitive) == 0)
		{
			selectedIndex = index;
			break;
		}
	}
	profileComboBox->setCurrentIndex(selectedIndex);
	refreshProfileMenus();
}

bool MainWindow::validateProfileName(const QString& name, QString* fileName) const
{
	QString candidate = name.trimmed();
	if (candidate.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive))
		candidate.chop(4);
	if (candidate.isEmpty() || candidate == QStringLiteral(".") || candidate == QStringLiteral(".."))
		return false;

	const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
	for (const QChar character : invalidCharacters)
	{
		if (candidate.contains(character))
			return false;
	}
	for (const QChar character : candidate)
	{
		if (character.unicode() < 0x20)
			return false;
	}
	if (candidate.endsWith(' ') || candidate.endsWith('.'))
		return false;
	const QString deviceStem = candidate.section('.', 0, 0).toUpper();
	static const QStringList reservedDeviceNames = {
		QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
		QStringLiteral("NUL"), QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
		QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"),
		QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6"),
		QStringLiteral("COM7"), QStringLiteral("COM8"), QStringLiteral("COM9"),
		QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
		QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"),
		QStringLiteral("LPT7"), QStringLiteral("LPT8"), QStringLiteral("LPT9")
	};
	if (reservedDeviceNames.contains(deviceStem))
		return false;

	*fileName = candidate + QStringLiteral(".txt");
	return true;
}

void MainWindow::profileSelected(int index)
{
	if (profileComboBox == NULL || index < 0 || index >= profileComboBox->count())
		return;
	if (!restoreTemporaryProcessingState())
	{
		syncProfileSelection();
		return;
	}
	const QString path = profileComboBox->itemData(index).toString();
	const bool opened = load(path);
	syncProfileSelection();
	refreshWorkspaceActionState();
	if (opened)
		showWorkspaceStatus(tr("Opened %1").arg(profileComboBox->itemText(index)));
}

void MainWindow::duplicateCurrentProfile()
{
	if (!restoreTemporaryProcessingState())
		return;
	FilterTable* filterTable = currentFilterTable();
	if (filterTable == NULL)
	{
		showWorkspaceStatus(tr("Open a profile before duplicating it"), "warning");
		return;
	}

	const QString baseName = currentProfilePath().isEmpty()
		? tr("New profile")
		: QFileInfo(currentProfilePath()).completeBaseName() + tr(" copy");
	bool accepted = false;
	const QString name = QInputDialog::getText(
		this, tr("Duplicate profile"), tr("Profile name:"),
		QLineEdit::Normal, baseName, &accepted);
	if (!accepted)
		return;

	QString fileName;
	if (!validateProfileName(name, &fileName))
	{
		QMessageBox::warning(this, tr("Invalid profile name"),
			tr("Use a file name without < > : \" / \\ | ? * and without a trailing space or period."));
		return;
	}
	const QString path = QDir::toNativeSeparators(configDir.absoluteFilePath(fileName));
	if (QFileInfo::exists(path))
	{
		QMessageBox::warning(this, tr("Profile already exists"),
			tr("A profile named %1 already exists.").arg(fileName));
		return;
	}

	if (!save(filterTable, path))
		return;
	refreshProfiles();
	if (!load(path))
		return;
	syncProfileSelection();
	showWorkspaceStatus(tr("Created %1").arg(QFileInfo(path).completeBaseName()));
}

void MainWindow::renameCurrentProfile()
{
	if (!restoreTemporaryProcessingState())
		return;
	FilterTable* filterTable = currentFilterTable();
	const QString oldPath = currentProfilePath();
	if (filterTable == NULL || oldPath.isEmpty())
	{
		showWorkspaceStatus(tr("Save the profile before renaming it"), "warning");
		return;
	}
	if (isFilterTableDirty(filterTable))
	{
		showWorkspaceStatus(tr("Save the profile before renaming it"), "warning");
		return;
	}
	if (QFileInfo(oldPath).absolutePath().compare(configDir.absolutePath(), Qt::CaseInsensitive) != 0)
	{
		showWorkspaceStatus(tr("Only profiles in the Hibiki EQAPO config folder can be renamed"), "warning");
		return;
	}
	if (QFileInfo(oldPath).fileName().compare(QStringLiteral("config.txt"), Qt::CaseInsensitive) == 0)
	{
		showWorkspaceStatus(tr("config.txt is the active root configuration and cannot be renamed here"), "warning");
		return;
	}

	bool accepted = false;
	const QString name = QInputDialog::getText(
		this, tr("Rename profile"), tr("New profile name:"),
		QLineEdit::Normal, QFileInfo(oldPath).completeBaseName(), &accepted);
	if (!accepted)
		return;
	QString fileName;
	if (!validateProfileName(name, &fileName))
	{
		QMessageBox::warning(this, tr("Invalid profile name"),
			tr("Use a valid Windows file name without a trailing space or period."));
		return;
	}
	const QString newPath = QDir::toNativeSeparators(configDir.absoluteFilePath(fileName));
	if (oldPath.compare(newPath, Qt::CaseInsensitive) == 0)
		return;
	if (QFileInfo::exists(newPath))
	{
		QMessageBox::warning(this, tr("Profile already exists"),
			tr("A profile named %1 already exists.").arg(fileName));
		return;
	}
	if (QMessageBox::question(this, tr("Rename profile"),
		tr("References to %1 inside other configuration files are not changed automatically. Rename it anyway?")
			.arg(QFileInfo(oldPath).fileName())) != QMessageBox::Yes)
		return;

	if (!QFile::rename(oldPath, newPath))
	{
		QMessageBox::critical(this, tr("Rename failed"),
			tr("Could not rename the profile. Check file permissions and try again."));
		return;
	}
	filterTable->setConfigPath(newPath);
	const int tabIndex = ui->tabWidget->currentIndex();
	ui->tabWidget->setTabText(tabIndex, QFileInfo(newPath).fileName());
	ui->tabWidget->setTabToolTip(tabIndex, newPath);
	for (QString& recentPath : recentFiles)
	{
		if (QDir::cleanPath(recentPath).compare(
			QDir::cleanPath(oldPath), Qt::CaseInsensitive) == 0)
			recentPath = newPath;
	}
	QSettings linkSettings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	linkSettings.beginGroup(QStringLiteral("profileDeviceLinks"));
	for (const QString& key : linkSettings.allKeys())
	{
		if (QDir::cleanPath(linkSettings.value(key).toString()).compare(
			QDir::cleanPath(oldPath), Qt::CaseInsensitive) == 0)
			linkSettings.setValue(key, newPath);
	}
	linkSettings.endGroup();
	linkSettings.sync();
	refreshProfiles();
	syncProfileSelection();
	showWorkspaceStatus(tr("Renamed profile to %1").arg(QFileInfo(newPath).completeBaseName()));
}

void MainWindow::importProfile()
{
	if (!restoreTemporaryProcessingState())
		return;
	const QString sourcePath = QFileDialog::getOpenFileName(
		this, tr("Import profile"), QString(), tr("E-APO configurations (*.txt)"));
	if (sourcePath.isEmpty())
		return;

	bool accepted = false;
	const QString name = QInputDialog::getText(
		this, tr("Import profile"), tr("Profile name:"), QLineEdit::Normal,
		QFileInfo(sourcePath).completeBaseName(), &accepted);
	if (!accepted)
		return;
	QString fileName;
	if (!validateProfileName(name, &fileName))
	{
		QMessageBox::warning(this, tr("Invalid profile name"), tr("Enter a valid Windows file name."));
		return;
	}
	const QString destinationPath = QDir::toNativeSeparators(configDir.absoluteFilePath(fileName));
	if (QFileInfo::exists(destinationPath))
	{
		QMessageBox::warning(this, tr("Profile already exists"),
			tr("A profile named %1 already exists.").arg(fileName));
		return;
	}
	if (!QFile::copy(sourcePath, destinationPath))
	{
		QMessageBox::critical(this, tr("Import failed"),
			tr("The profile could not be copied into the Hibiki EQAPO config folder."));
		return;
	}
	refreshProfiles();
	if (!load(destinationPath))
		return;
	syncProfileSelection();
	showWorkspaceStatus(tr("Imported %1").arg(QFileInfo(destinationPath).completeBaseName()));
}

void MainWindow::exportCurrentProfile()
{
	if (!restoreTemporaryProcessingState())
		return;
	FilterTable* filterTable = currentFilterTable();
	if (filterTable == NULL)
	{
		showWorkspaceStatus(tr("Open a profile before exporting it"), "warning");
		return;
	}
	QString suggestedName = currentProfilePath().isEmpty()
		? QStringLiteral("profile.txt") : QFileInfo(currentProfilePath()).fileName();
	const QString destinationPath = QFileDialog::getSaveFileName(
		this, tr("Export profile"), suggestedName, tr("E-APO configurations (*.txt)"));
	if (destinationPath.isEmpty())
		return;
	QString path = destinationPath;
	if (!path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive))
		path += QStringLiteral(".txt");
	if (!save(filterTable, QDir::toNativeSeparators(path)))
		return;
	showWorkspaceStatus(tr("Exported profile to %1").arg(QFileInfo(path).fileName()));
}

QString MainWindow::deviceLinkKey() const
{
	shared_ptr<AbstractAPOInfo> selectedDevice;
	int channelMask;
	const_cast<MainWindow*>(this)->getDeviceAndChannelMask(&selectedDevice, &channelMask);
	Q_UNUSED(channelMask);
	if (selectedDevice == NULL)
		return QString();
	const QByteArray identity = QString::fromStdWString(selectedDevice->getDeviceString()).toUtf8();
	return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QString MainWindow::linkedProfileForCurrentDevice() const
{
	const QString key = deviceLinkKey();
	if (key.isEmpty())
		return QString();
	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	return settings.value(QStringLiteral("profileDeviceLinks/") + key).toString();
}

void MainWindow::linkCurrentProfileToDevice()
{
	const QString path = currentProfilePath();
	const QString key = deviceLinkKey();
	if (path.isEmpty() || key.isEmpty())
	{
		showWorkspaceStatus(tr("Select a saved profile and playback device first"), "warning");
		return;
	}
	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	settings.setValue(QStringLiteral("profileDeviceLinks/") + key, path);
	showWorkspaceStatus(tr("Linked %1 to the selected device").arg(QFileInfo(path).completeBaseName()));
}

void MainWindow::clearCurrentDeviceProfileLink()
{
	const QString key = deviceLinkKey();
	if (key.isEmpty())
	{
		showWorkspaceStatus(tr("Select a playback device first"), "warning");
		return;
	}
	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	settings.remove(QStringLiteral("profileDeviceLinks/") + key);
	showWorkspaceStatus(tr("Cleared the selected device profile link"));
}

void MainWindow::findNext()
{
	FilterTable* filterTable = currentFilterTable();
	if (filterTable == NULL || searchLineEdit == NULL)
		return;
	const QString text = searchLineEdit->text().trimmed();
	if (text.isEmpty())
	{
		searchLineEdit->setFocus(Qt::ShortcutFocusReason);
		return;
	}
	const int row = filterTable->findText(text, false);
	if (row < 0)
		showWorkspaceStatus(tr("No filter or setting matches “%1”").arg(text), "warning");
	else
		showWorkspaceStatus(tr("Match on row %1").arg(row + 1), "normal", 2200);
}

void MainWindow::findPrevious()
{
	FilterTable* filterTable = currentFilterTable();
	if (filterTable == NULL || searchLineEdit == NULL)
		return;
	const QString text = searchLineEdit->text().trimmed();
	if (text.isEmpty())
	{
		searchLineEdit->setFocus(Qt::ShortcutFocusReason);
		return;
	}
	const int row = filterTable->findText(text, true);
	if (row < 0)
		showWorkspaceStatus(tr("No filter or setting matches “%1”").arg(text), "warning");
	else
		showWorkspaceStatus(tr("Match on row %1").arg(row + 1), "normal", 2200);
}

void MainWindow::searchTextChanged(const QString& text)
{
	if (text.trimmed().isEmpty())
	{
		if (FilterTable* filterTable = currentFilterTable())
			filterTable->clearFindSelection();
		return;
	}
	if (text.trimmed().size() >= 2)
		findNext();
}

bool MainWindow::prepareTemporaryContents(
	FilterTable* filterTable,
	const QList<QString>& expectedLines,
	const QList<QString>& temporaryLines,
	QByteArray* originalContent,
	QByteArray* temporaryContent,
	bool* externalConflict)
{
	if (externalConflict != NULL)
		*externalConflict = false;
	if (filterTable == NULL || filterTable->getConfigPath().isEmpty()
		|| originalContent == NULL || temporaryContent == NULL)
		return false;

	const QString path = filterTable->getConfigPath();
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;
	const QByteArray observedContent = file.readAll();
	const bool readSucceeded = file.error() == QFileDevice::NoError;
	file.close();
	if (!readSucceeded)
		return false;

	if (deserializeConfigurationLines(observedContent) != expectedLines)
	{
		if (externalConflict != NULL)
			*externalConflict = true;
		QScopedValueRollback<bool> restoringGuard(restoringTemporaryState, true);
		if (filterTable->setLines(path, deserializeConfigurationLines(observedContent)))
		{
			filterTable->updateAnalysis();
			refreshProfiles();
		}
		return false;
	}

	*originalContent = observedContent;
	*temporaryContent = serializeConfigurationLinesLike(temporaryLines, observedContent);
	return true;
}

bool MainWindow::beginTemporaryFilterConfiguration(
	IFilterGUI* filterGui,
	const QString& command,
	const QString& parameters,
	const QString& mode)
{
	if (filterGui == NULL || !temporaryFilterTable.isNull())
		return false;
	const bool midiLearning = mode == QStringLiteral("vst-midi-learn");

	TemporaryProcessingMutexGuard processingLock;
	if (!processingLock.isLocked())
	{
		showWorkspaceStatus(
			tr("A temporary audio state could not be activated"),
			"danger", 0);
		return false;
	}

	FilterTable* filterTable = currentFilterTable();
	if (filterTable == NULL || filterTable->getConfigPath().isEmpty())
	{
		showWorkspaceStatus(
			midiLearning
				? tr("Save the profile before configuring MIDI")
				: tr("Save the profile before starting calibration"),
			"warning");
		return false;
	}
	// This first phase is read-only. The external rows are committed only after
	// the temporary file write has succeeded, so a write failure cannot close a
	// panel while leaving the requested operation canceled.
	if (!filterTable->prepareDeleteAllItems() || isFilterTableDirty(filterTable))
	{
		showWorkspaceStatus(
			midiLearning
				? tr("Save the profile before configuring MIDI")
				: tr("Save the profile before starting calibration"),
			"warning");
		return false;
	}

	recoverInterruptedTemporaryProcessingState();
	if (hasConflictingTemporaryJournal(
		filterTable->getConfigPath(), temporaryRecoveryOwner))
	{
		showWorkspaceStatus(
			tr("A temporary audio state could not be recovered automatically"),
			"danger", 0);
		return false;
	}

	const QList<QString> originalLines = filterTable->getLines();
	QList<QString> temporaryLines;
	if (!filterTable->makeLinesWithGuiOverride(
		filterGui, command, parameters, &temporaryLines))
	{
		return false;
	}

	QFile file(filterTable->getConfigPath());
	if (!file.open(QIODevice::ReadOnly))
		return false;
	const QByteArray originalContent = file.readAll();
	const bool readSucceeded = file.error() == QFileDevice::NoError;
	file.close();
	if (!readSucceeded || deserializeConfigurationLines(originalContent) != originalLines)
	{
		showWorkspaceStatus(
			midiLearning
				? tr("The profile changed outside the editor; MIDI configuration was not started")
				: tr("The profile changed outside the editor; calibration was not started"),
			"warning");
		return false;
	}

	const QByteArray temporaryContent = serializeConfigurationLinesLike(
		temporaryLines, originalContent);
	if (!writeTemporaryRecoveryJournal(
		filterTable, originalLines, originalContent, temporaryContent, mode))
	{
		showWorkspaceStatus(
			tr("A temporary audio state could not be activated"),
			"danger", 0);
		return false;
	}

	const ConditionalWriteResult result = conditionallyWriteConfiguration(
		filterTable->getConfigPath(), originalContent, temporaryContent);
	if (result.status != ConditionalWriteStatus::written)
	{
		if (result.status == ConditionalWriteStatus::conflict)
		{
			// No write was attempted, so this journal never became recovery state.
			clearTemporaryRecoveryJournal();
			showWorkspaceStatus(
				midiLearning
					? tr("The profile changed outside the editor; MIDI configuration was not started")
					: tr("The profile changed outside the editor; calibration was not started"),
				"warning", 4500);
		}
		else if (result.rollbackVerified)
		{
			// A failed write may have been partial. Only discard the sole recovery
			// record after rereading the file and proving the original bytes won.
			clearTemporaryRecoveryJournal();
			showWorkspaceStatus(
				tr("A temporary audio state could not be activated"),
				"danger", 0);
		}
		else
		{
			showWorkspaceStatus(
				tr("A temporary audio state could not be recovered automatically"),
				"danger", 0);
			QMessageBox::critical(
				this,
				tr("Audio processing was not restored"),
				tr("A temporary audio state could not be recovered automatically"));
		}
		return false;
	}

	bool deleteCommitted = false;
	{
		// A successful commit may publish recovered plug-in state. Let the tab
		// become dirty, but keep Instant mode from overwriting the temporary
		// file before this transaction completes or rolls back.
		QScopedValueRollback<bool> instantSaveGuard(suppressInstantSave, true);
		deleteCommitted = filterTable->commitDeleteAllItems();
	}
	const bool recoveredStateChanged = filterTable->getLines() != originalLines;
	if (!deleteCommitted || recoveredStateChanged)
	{
		const bool rollbackVerified = rollbackConditionalConfigurationWrite(
			filterTable->getConfigPath(), temporaryContent, originalContent);
		if (rollbackVerified)
			clearTemporaryRecoveryJournal();
		showWorkspaceStatus(
			recoveredStateChanged
				? (midiLearning
					? tr("Save the profile before configuring MIDI")
					: tr("Save the profile before starting calibration"))
				: tr("A temporary audio state could not be activated"),
			rollbackVerified ? "warning" : "danger", 0);
		return false;
	}

	temporaryFilterTable = filterTable;
	temporaryFilterOriginalLines = originalLines;
	temporaryFilterOriginalContent = originalContent;
	temporaryFilterContent = temporaryContent;
	showWorkspaceStatus(
		midiLearning
			? tr("This row temporarily released its MIDI input")
			: tr("Calibration contour is temporarily bypassed"),
		"warning", 0);
	return true;
}

bool MainWindow::restoreTemporaryFilterConfiguration(bool* keptExternal)
{
	if (keptExternal != NULL)
		*keptExternal = false;
	if (temporaryFilterTable.isNull())
		return true;

	TemporaryProcessingMutexGuard processingLock;
	if (!processingLock.isLocked())
		return false;

	QPointer<FilterTable> filterTable = temporaryFilterTable;
	const QString path = filterTable->getConfigPath();
	QByteArray expectedContent = temporaryFilterContent;
	bool keepExternal = false;
	QByteArray externalContent;

	for (int attempt = 0; attempt < 4; ++attempt)
	{
		const ConditionalWriteResult result = conditionallyWriteConfiguration(
			path, expectedContent, temporaryFilterOriginalContent);
		if (result.status == ConditionalWriteStatus::written
			|| (result.status == ConditionalWriteStatus::conflict
				&& result.currentContent == temporaryFilterOriginalContent))
		{
			break;
		}
		if (result.status == ConditionalWriteStatus::failed)
		{
			showWorkspaceStatus(
				tr("The current profile could not be read; temporary audio remains active"),
				"danger", 0);
			return false;
		}

		QMessageBox conflictBox(
			QMessageBox::Warning,
			tr("External profile change"),
			tr("This profile changed in another program while temporary audio was active. Restore saved profile will overwrite those external changes."),
			QMessageBox::NoButton,
			this);
		QPushButton* restoreButton = conflictBox.addButton(
			tr("Restore saved profile"), QMessageBox::AcceptRole);
		QPushButton* keepButton = conflictBox.addButton(
			tr("Keep external changes"), QMessageBox::RejectRole);
		conflictBox.setDefaultButton(keepButton);
		conflictBox.setEscapeButton(keepButton);
		conflictBox.exec();
		if (conflictBox.clickedButton() != restoreButton)
		{
			keepExternal = true;
			externalContent = result.currentContent;
			break;
		}
		expectedContent = result.currentContent;
		if (attempt == 3)
			return false;
	}

	clearTemporaryRecoveryJournal();
	temporaryFilterTable.clear();
	temporaryFilterOriginalLines.clear();
	temporaryFilterOriginalContent.clear();
	temporaryFilterContent.clear();
	showWorkspaceStatus(
		keepExternal ? tr("Kept external profile changes") : tr("Audio processing restored"),
		keepExternal ? "warning" : "normal");

	if (keepExternal && !filterTable.isNull())
	{
		// Defer rebuilding the rows until the filter GUI that requested the
		// modal calibration has returned from its slot.
		QTimer::singleShot(0, this, [this, filterTable, path, externalContent]() {
			if (filterTable.isNull())
				return;
			QScopedValueRollback<bool> restoringGuard(restoringTemporaryState, true);
			if (filterTable->setLines(path, deserializeConfigurationLines(externalContent)))
			{
				filterTable->updateAnalysis();
				refreshProfiles();
			}
		});
	}
	if (keptExternal != NULL)
		*keptExternal = keepExternal;
	return true;
}

bool MainWindow::setTemporaryLines(
	FilterTable* filterTable,
	const QList<QString>& lines,
	const QByteArray& expectedContent,
	const QByteArray& temporaryContent,
	bool* externalConflict)
{
	if (externalConflict != NULL)
		*externalConflict = false;
	if (filterTable == NULL || filterTable->getConfigPath().isEmpty())
		return false;
	const QString path = filterTable->getConfigPath();
	// Do not write a temporary profile that the editor cannot safely mirror.
	// Preflight is deliberately side-effect free; lifecycle cleanup is
	// committed only after the conditional write succeeds.
	if (!filterTable->prepareDeleteAllItems())
		return false;
	const QList<QString> linesBeforeDeleteCommit = filterTable->getLines();
	const ConditionalWriteResult result = conditionallyWriteConfiguration(
		path, expectedContent, temporaryContent);
	if (result.status == ConditionalWriteStatus::failed)
		return false;

	if (result.status == ConditionalWriteStatus::conflict)
	{
		if (externalConflict != NULL)
			*externalConflict = true;
		QScopedValueRollback<bool> restoringGuard(restoringTemporaryState, true);
		if (filterTable->setLines(path, deserializeConfigurationLines(result.currentContent)))
		{
			filterTable->updateAnalysis();
			refreshProfiles();
		}
		return false;
	}

	bool deleteCommitted = false;
	{
		// State recovered while committing must remain visibly dirty, but an
		// Instant-mode save here would destroy the conditional-write contract.
		QScopedValueRollback<bool> instantSaveGuard(suppressInstantSave, true);
		deleteCommitted = filterTable->commitDeleteAllItems();
	}
	const bool recoveredStateChanged =
		filterTable->getLines() != linesBeforeDeleteCommit;
	if (!deleteCommitted || recoveredStateChanged)
	{
		const bool rollbackVerified = rollbackConditionalConfigurationWrite(
			path, temporaryContent, expectedContent);
		if (externalConflict != NULL && recoveredStateChanged && rollbackVerified)
			*externalConflict = true;
		return false;
	}
	{
		QScopedValueRollback<bool> restoringGuard(restoringTemporaryState, true);
		filterTable->setLinesAfterDeleteCommit(path, lines);
		if (filterTable->getLines() != lines)
			return false;
	}
	filterTable->updateAnalysis();
	refreshProfiles();
	return true;
}

bool MainWindow::restoreTemporaryLines(
	FilterTable* filterTable,
	const QList<QString>& originalLines,
	const QByteArray& originalContent,
	const QByteArray& expectedTemporaryContent)
{
	if (filterTable == NULL || filterTable->getConfigPath().isEmpty())
		return false;

	const QString path = filterTable->getConfigPath();
	QByteArray expectedContent = expectedTemporaryContent;
	if (filterTable->getLines() != originalLines &&
		!filterTable->prepareDeleteAllItems())
		return false;

	auto updateTable = [this, filterTable, &path](const QList<QString>& lines)
	{
		if (filterTable->getLines() == lines)
			return true;
		QScopedValueRollback<bool> restoringGuard(restoringTemporaryState, true);
		if (!filterTable->setLines(path, lines))
			return false;
		filterTable->updateAnalysis();
		refreshProfiles();
		return true;
	};

	for (int attempt = 0; attempt < 4; ++attempt)
	{
		const ConditionalWriteResult result = conditionallyWriteConfiguration(
			path, expectedContent, originalContent);
		if (result.status == ConditionalWriteStatus::written)
		{
			return updateTable(originalLines);
		}
		if (result.status == ConditionalWriteStatus::failed)
		{
			showWorkspaceStatus(
				tr("The current profile could not be read; temporary audio remains active"),
				"danger", 0);
			return false;
		}

		if (result.currentContent == originalContent)
		{
			return updateTable(originalLines);
		}

		QMessageBox conflictBox(
			QMessageBox::Warning,
			tr("External profile change"),
			tr("This profile changed in another program while temporary audio was active. Restore saved profile will overwrite those external changes."),
			QMessageBox::NoButton,
			this);
		QPushButton* restoreButton = conflictBox.addButton(
			tr("Restore saved profile"), QMessageBox::AcceptRole);
		QPushButton* keepButton = conflictBox.addButton(
			tr("Keep external changes"), QMessageBox::RejectRole);
		conflictBox.setDefaultButton(keepButton);
		conflictBox.setEscapeButton(keepButton);
		conflictBox.exec();
		if (conflictBox.clickedButton() != restoreButton)
		{
			if (!updateTable(deserializeConfigurationLines(result.currentContent)))
				return false;
			showWorkspaceStatus(tr("Kept external profile changes"), "warning");
			return true;
		}
		expectedContent = result.currentContent;
	}

	showWorkspaceStatus(
		tr("A temporary audio state could not be recovered automatically"),
		"danger", 0);
	return false;
}

bool MainWindow::writeTemporaryRecoveryJournal(
	FilterTable* filterTable,
	const QList<QString>& originalLines,
	const QByteArray& originalContent,
	const QByteArray& temporaryContent,
	const QString& mode)
{
	if (filterTable == NULL || filterTable->getConfigPath().isEmpty())
		return false;
	const QByteArray originalHash = QCryptographicHash::hash(
		originalContent, QCryptographicHash::Sha256);
	const QByteArray temporaryHash = QCryptographicHash::hash(
		temporaryContent, QCryptographicHash::Sha256);
	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	settings.beginGroup(QStringLiteral("temporaryProcessing"));
	settings.beginGroup(temporaryRecoveryOwner);
	settings.remove(QString());
	settings.setValue(QStringLiteral("journalVersion"), 2);
	settings.setValue(QStringLiteral("committed"), false);
	settings.setValue(QStringLiteral("path"), filterTable->getConfigPath());
	settings.setValue(QStringLiteral("lines"), QStringList(originalLines.begin(), originalLines.end()));
	settings.setValue(QStringLiteral("originalContent"), originalContent);
	settings.setValue(QStringLiteral("originalHash"), originalHash);
	settings.setValue(QStringLiteral("temporaryHash"), temporaryHash);
	settings.setValue(QStringLiteral("temporaryContent"), temporaryContent);
	settings.setValue(QStringLiteral("mode"), mode);
	settings.setValue(QStringLiteral("createdAtUtcMs"), QDateTime::currentMSecsSinceEpoch());
	settings.setValue(QStringLiteral("ownerProcessId"),
		static_cast<uint>(GetCurrentProcessId()));
	settings.setValue(QStringLiteral("ownerProcessStartedAt"), temporaryRecoveryProcessStartedAt);
	settings.sync();
	if (settings.status() != QSettings::NoError)
	{
		settings.endGroup();
		settings.endGroup();
		return false;
	}
	settings.setValue(QStringLiteral("committed"), true);
	settings.sync();
	const bool succeeded = settings.status() == QSettings::NoError;
	settings.endGroup();
	settings.endGroup();
	return succeeded;
}

void MainWindow::clearTemporaryRecoveryJournal()
{
	TemporaryProcessingMutexGuard processingLock;
	if (!processingLock.isLocked())
		return;
	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	settings.beginGroup(QStringLiteral("temporaryProcessing"));
	settings.remove(temporaryRecoveryOwner);
	settings.endGroup();
	settings.sync();
}

void MainWindow::recoverInterruptedTemporaryProcessingState()
{
	TemporaryProcessingMutexGuard processingLock;
	if (!processingLock.isLocked())
	{
		showWorkspaceStatus(
			tr("A temporary audio state could not be recovered automatically"),
			"danger", 0);
		return;
	}

	struct RecoveryRecord
	{
		QString owner;
		QString path;
		QByteArray originalContent;
		QByteArray originalHash;
		QByteArray temporaryContent;
		QByteArray temporaryHash;
		bool hasTemporaryContent = false;
		bool valid = true;
		JournalOwnerStatus ownerStatus = JournalOwnerStatus::dead;
	};

	auto clearJournal = [](const QString& owner) {
		QSettings cleanup(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
		cleanup.beginGroup(QStringLiteral("temporaryProcessing"));
		if (owner.isEmpty())
		{
			for (const QString& key : {
				QStringLiteral("path"), QStringLiteral("lines"),
				QStringLiteral("originalContent"),
				QStringLiteral("originalHash"), QStringLiteral("temporaryHash"),
				QStringLiteral("temporaryContent"),
				QStringLiteral("mode"), QStringLiteral("createdAtUtcMs"),
				QStringLiteral("ownerProcessId"),
				QStringLiteral("ownerProcessStartedAt"),
				QStringLiteral("journalVersion"), QStringLiteral("committed") })
				cleanup.remove(key);
		}
		else
		{
			cleanup.remove(owner);
		}
		cleanup.endGroup();
		cleanup.sync();
	};

	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	settings.beginGroup(QStringLiteral("temporaryProcessing"));
	QStringList owners = settings.childGroups();
	const bool hasLegacyJournal = settings.contains(QStringLiteral("path"));
	settings.endGroup();
	if (hasLegacyJournal)
		owners.prepend(QString());

	QMap<QString, QVector<RecoveryRecord>> recordsByPath;
	for (const QString& owner : owners)
	{
		QSettings journal(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
		journal.beginGroup(QStringLiteral("temporaryProcessing"));
		if (!owner.isEmpty())
			journal.beginGroup(owner);
		RecoveryRecord record;
		record.owner = owner;
		const int journalVersion = journal.value(
			QStringLiteral("journalVersion")).toInt();
		const bool committed = journal.value(
			QStringLiteral("committed"), journalVersion < 2).toBool();
		record.path = journal.value(QStringLiteral("path")).toString();
		const QVariant linesValue = journal.value(QStringLiteral("lines"));
		const bool hasOriginalContent = journal.contains(QStringLiteral("originalContent"));
		const QByteArray storedOriginalContent = journal.value(
			QStringLiteral("originalContent")).toByteArray();
		const QByteArray recordedOriginalHash = journal.value(
			QStringLiteral("originalHash")).toByteArray();
		record.temporaryHash = journal.value(
			QStringLiteral("temporaryHash")).toByteArray();
		record.hasTemporaryContent = journal.contains(QStringLiteral("temporaryContent"));
		record.temporaryContent = journal.value(
			QStringLiteral("temporaryContent")).toByteArray();
		const quint32 ownerProcessId = journal.value(
			QStringLiteral("ownerProcessId")).toUInt();
		const quint64 ownerProcessStartedAt = journal.value(
			QStringLiteral("ownerProcessStartedAt")).toULongLong();
		if (!owner.isEmpty())
			journal.endGroup();
		journal.endGroup();

		if (journalVersion >= 2 && !committed)
		{
			clearJournal(owner);
			continue;
		}
		if (record.path.isEmpty())
		{
			clearJournal(owner);
			continue;
		}

		const QStringList lines = linesValue.toStringList();
		record.valid = hasOriginalContent || linesValue.isValid();
		record.originalContent = hasOriginalContent
			? storedOriginalContent : serializeConfigurationLines(lines);
		record.originalHash = QCryptographicHash::hash(
			record.originalContent, QCryptographicHash::Sha256);
		record.valid = record.valid && (recordedOriginalHash.isEmpty()
			|| recordedOriginalHash == record.originalHash);
		if (record.hasTemporaryContent)
		{
			const QByteArray computedTemporaryHash = QCryptographicHash::hash(
				record.temporaryContent, QCryptographicHash::Sha256);
			record.valid = record.valid && (record.temporaryHash.isEmpty()
				|| record.temporaryHash == computedTemporaryHash);
			record.temporaryHash = computedTemporaryHash;
		}
		record.ownerStatus = journalOwnerStatus(
			ownerProcessId, ownerProcessStartedAt);

		recordsByPath[configurationPathKey(record.path)].append(record);
	}

	for (auto pathIt = recordsByPath.cbegin(); pathIt != recordsByPath.cend(); ++pathIt)
	{
		QVector<RecoveryRecord> pending = pathIt.value();
		if (pending.isEmpty())
			continue;

		bool deferred = false;
		for (const RecoveryRecord& record : pending)
		{
			if (!record.valid || record.ownerStatus != JournalOwnerStatus::dead)
			{
				deferred = true;
				break;
			}
		}
		if (deferred)
		{
			if (std::any_of(pending.cbegin(), pending.cend(), [](const RecoveryRecord& record)
				{ return !record.valid; }))
			{
				showWorkspaceStatus(
					tr("A temporary audio state could not be recovered automatically"),
					"danger", 0);
			}
			continue;
		}

		const QString path = pending.first().path;
		QFile currentFile(path);
		if (!currentFile.open(QIODevice::ReadOnly))
		{
			showWorkspaceStatus(
				tr("A temporary audio state could not be recovered automatically"),
				"danger", 0);
			continue;
		}
		QByteArray currentContent = currentFile.readAll();
		currentFile.close();
		bool recovered = false;
		bool failed = false;
		bool postponed = false;

		for (int pass = 0; !pending.isEmpty() && pass < 32; ++pass)
		{
			const QByteArray currentHash = QCryptographicHash::hash(
				currentContent, QCryptographicHash::Sha256);

			QSet<QByteArray> resolvedHashes;
			resolvedHashes.insert(currentHash);
			bool removedResolved = false;
			do
			{
				removedResolved = false;
				for (int index = pending.size() - 1; index >= 0; --index)
				{
					if (!resolvedHashes.contains(pending[index].originalHash))
						continue;
					if (!pending[index].temporaryHash.isEmpty())
						resolvedHashes.insert(pending[index].temporaryHash);
					clearJournal(pending[index].owner);
					pending.removeAt(index);
					removedResolved = true;
				}
			} while (removedResolved);
			if (pending.isEmpty())
				break;

			QVector<int> matchingTemporary;
			for (int index = 0; index < pending.size(); ++index)
			{
				const bool matches = pending[index].hasTemporaryContent
					? pending[index].temporaryContent == currentContent
					: (!pending[index].temporaryHash.isEmpty()
						&& pending[index].temporaryHash == currentHash);
				if (matches)
					matchingTemporary.append(index);
			}

			if (!matchingTemporary.isEmpty())
			{
				const QByteArray originalContent = pending[matchingTemporary.first()].originalContent;
				const bool ambiguous = std::any_of(
					matchingTemporary.cbegin(), matchingTemporary.cend(),
					[&](int index) { return pending[index].originalContent != originalContent; });
				if (!ambiguous)
				{
					const ConditionalWriteResult result = conditionallyWriteConfiguration(
						path, currentContent, originalContent);
					if (result.status == ConditionalWriteStatus::conflict)
					{
						currentContent = result.currentContent;
						continue;
					}
					if (result.status == ConditionalWriteStatus::failed)
					{
						failed = true;
						break;
					}
					for (int match = matchingTemporary.size() - 1; match >= 0; --match)
					{
						const int index = matchingTemporary[match];
						clearJournal(pending[index].owner);
						pending.removeAt(index);
					}
					currentContent = originalContent;
					recovered = true;
					continue;
				}
			}

			QSet<QByteArray> temporaryHashes;
			for (const RecoveryRecord& record : pending)
				if (!record.temporaryHash.isEmpty())
					temporaryHashes.insert(record.temporaryHash);
			QByteArray rootContent;
			bool hasRoot = false;
			bool ambiguousRoot = false;
			for (const RecoveryRecord& record : pending)
			{
				if (temporaryHashes.contains(record.originalHash))
					continue;
				if (!hasRoot)
				{
					rootContent = record.originalContent;
					hasRoot = true;
				}
				else if (rootContent != record.originalContent)
				{
					ambiguousRoot = true;
				}
			}
			if (!hasRoot || ambiguousRoot)
			{
				failed = true;
				break;
			}

			QMessageBox recoveryBox(
				QMessageBox::Warning,
				tr("Temporary audio recovery"),
				tr("This profile changed in another program while temporary audio was active. Restore saved profile will overwrite those external changes."),
				QMessageBox::Cancel,
				this);
			QPushButton* restoreButton = recoveryBox.addButton(
				tr("Restore saved profile"), QMessageBox::AcceptRole);
			QPushButton* keepButton = recoveryBox.addButton(
				tr("Keep external changes"), QMessageBox::DestructiveRole);
			recoveryBox.setDefaultButton(QMessageBox::Cancel);
			recoveryBox.exec();
			if (recoveryBox.clickedButton() == keepButton)
			{
				for (const RecoveryRecord& record : pending)
					clearJournal(record.owner);
				pending.clear();
				showWorkspaceStatus(
					tr("Kept the current profile; no file was overwritten"),
					"warning");
				break;
			}
			if (recoveryBox.clickedButton() != restoreButton)
			{
				postponed = true;
				break;
			}

			const ConditionalWriteResult result = conditionallyWriteConfiguration(
				path, currentContent, rootContent);
			if (result.status == ConditionalWriteStatus::conflict)
			{
				currentContent = result.currentContent;
				continue;
			}
			if (result.status == ConditionalWriteStatus::failed)
			{
				failed = true;
				break;
			}
			for (const RecoveryRecord& record : pending)
				clearJournal(record.owner);
			pending.clear();
			currentContent = rootContent;
			recovered = true;
		}

		if (postponed)
		{
			showWorkspaceStatus(
				tr("Kept the current profile; no file was overwritten"),
				"warning");
		}
		else if (failed || !pending.isEmpty())
		{
			showWorkspaceStatus(
				tr("A temporary audio state could not be recovered automatically"),
				"danger", 0);
		}
		else if (recovered)
		{
			showWorkspaceStatus(
				tr("Recovered the profile from an interrupted temporary audio state"));
		}
	}
}

void MainWindow::captureComparisonA()
{
	FilterTable* filterTable = currentFilterTable();
	if (filterTable == NULL || filterTable->getConfigPath().isEmpty())
	{
		showWorkspaceStatus(tr("Save the profile before starting an audible A/B comparison"), "warning");
		return;
	}
	// Validate every external row before taking the snapshot. Lifecycle state is
	// committed only if a later temporary-file swap actually succeeds.
	if (!filterTable->prepareDeleteAllItems() || isFilterTableDirty(filterTable))
	{
		showWorkspaceStatus(tr("Save the profile before starting an audible A/B comparison"), "warning");
		return;
	}
	if (bypassAction != NULL && bypassAction->isChecked())
	{
		bypassAction->setChecked(false);
		if (bypassAction->isChecked())
			return;
	}
	if (showingComparisonA)
	{
		comparisonAction->setChecked(false);
		if (showingComparisonA)
			return;
	}

	comparisonTable = filterTable;
	comparisonALines = filterTable->getLines();
	comparisonBLines.clear();
	comparisonOriginalContent.clear();
	comparisonTemporaryContent.clear();
	showingComparisonA = false;
	{
		QSignalBlocker blocker(comparisonAction);
		comparisonAction->setChecked(false);
	}
	comparisonAction->setText(tr("Compare A"));
	comparisonAction->setEnabled(true);
	showWorkspaceStatus(tr("Captured comparison A. Make changes, then choose Compare A."));
}

void MainWindow::comparisonToggled(bool showA)
{
	if (restoringTemporaryState)
		return;
	FilterTable* filterTable = currentFilterTable();
	if (comparisonTable.isNull() || filterTable != comparisonTable)
	{
		QSignalBlocker blocker(comparisonAction);
		comparisonAction->setChecked(false);
		showWorkspaceStatus(tr("Comparison A belongs to another profile. Capture it again here."), "warning");
		return;
	}
	TemporaryProcessingMutexGuard processingLock;
	if (!processingLock.isLocked())
	{
		QSignalBlocker blocker(comparisonAction);
		comparisonAction->setChecked(showingComparisonA);
		showWorkspaceStatus(
			tr("A temporary audio state could not be recovered automatically"),
			"danger", 0);
		return;
	}

	if (showA)
	{
		// Validate external rows before taking B or creating recovery bytes.
		// setTemporaryLines commits them only after its conditional write.
		if (!filterTable->prepareDeleteAllItems() ||
			isFilterTableDirty(filterTable))
		{
			QSignalBlocker blocker(comparisonAction);
			comparisonAction->setChecked(false);
			showWorkspaceStatus(
				tr("Save the profile before starting an audible A/B comparison"),
				"warning");
			return;
		}
		if (bypassAction != NULL && bypassAction->isChecked())
		{
			bypassAction->setChecked(false);
			if (bypassAction->isChecked())
			{
				QSignalBlocker blocker(comparisonAction);
				comparisonAction->setChecked(false);
				return;
			}
		}
		recoverInterruptedTemporaryProcessingState();
		if (hasConflictingTemporaryJournal(
			filterTable->getConfigPath(), temporaryRecoveryOwner))
		{
			QSignalBlocker blocker(comparisonAction);
			comparisonAction->setChecked(false);
			showWorkspaceStatus(
				tr("A temporary audio state could not be recovered automatically"),
				"danger", 0);
			return;
		}
		comparisonBLines = filterTable->getLines();
		bool externalConflict = false;
		if (!prepareTemporaryContents(
			filterTable, comparisonBLines, comparisonALines,
			&comparisonOriginalContent, &comparisonTemporaryContent,
			&externalConflict))
		{
			QSignalBlocker blocker(comparisonAction);
			comparisonAction->setChecked(false);
			showWorkspaceStatus(
				externalConflict
					? tr("Kept external profile changes")
					: tr("Could not activate comparison A"),
				externalConflict ? "warning" : "danger",
				externalConflict ? 4500 : 0);
			return;
		}
		if (!writeTemporaryRecoveryJournal(
			filterTable, comparisonBLines, comparisonOriginalContent,
			comparisonTemporaryContent, QStringLiteral("comparison")))
		{
			QSignalBlocker blocker(comparisonAction);
			comparisonAction->setChecked(false);
			showWorkspaceStatus(tr("Could not activate comparison A"), "danger", 0);
			return;
		}
		if (!setTemporaryLines(
			filterTable, comparisonALines, comparisonOriginalContent,
			comparisonTemporaryContent, &externalConflict))
		{
			const bool restored = externalConflict || restoreTemporaryLines(
				filterTable, comparisonBLines, comparisonOriginalContent,
				comparisonTemporaryContent);
			if (!restored)
			{
				filterTable->setEnabled(false);
				showingComparisonA = true;
				comparisonAction->setText(tr("Return to B"));
				QSignalBlocker blocker(comparisonAction);
				comparisonAction->setChecked(true);
				showWorkspaceStatus(
					tr("A temporary audio state could not be recovered automatically"),
					"danger", 0);
				refreshWorkspaceActionState();
				return;
			}
			clearTemporaryRecoveryJournal();
			filterTable->setEnabled(true);
			showingComparisonA = false;
			comparisonOriginalContent.clear();
			comparisonTemporaryContent.clear();
			comparisonAction->setText(tr("Compare A"));
			QSignalBlocker blocker(comparisonAction);
			comparisonAction->setChecked(false);
			showWorkspaceStatus(
				externalConflict
					? tr("Kept external profile changes")
					: tr("Could not activate comparison A"),
				externalConflict ? "warning" : "danger",
				externalConflict ? 4500 : 0);
			return;
		}
		filterTable->setEnabled(false);
		showingComparisonA = true;
		comparisonAction->setText(tr("Return to B"));
		showWorkspaceStatus(tr("Listening to captured A"), "warning", 0);
	}
	else
	{
		const bool restored = !showingComparisonA
			|| restoreTemporaryLines(
				filterTable, comparisonBLines, comparisonOriginalContent,
				comparisonTemporaryContent);
		if (restored)
			clearTemporaryRecoveryJournal();
		else
		{
			QSignalBlocker blocker(comparisonAction);
			comparisonAction->setChecked(true);
			comparisonAction->setText(tr("Return to B"));
			filterTable->setEnabled(false);
			showWorkspaceStatus(
				tr("A temporary audio state could not be recovered automatically"),
				"danger", 0);
			refreshWorkspaceActionState();
			return;
		}
		filterTable->setEnabled(true);
		showingComparisonA = false;
		comparisonOriginalContent.clear();
		comparisonTemporaryContent.clear();
		comparisonAction->setText(tr("Compare A"));
		showWorkspaceStatus(restored
			? tr("Returned to current B")
			: tr("A temporary audio state could not be recovered automatically"),
			restored ? "normal" : "danger", restored ? 3500 : 0);
	}
	refreshWorkspaceActionState();
}

void MainWindow::bypassToggled(bool enabled)
{
	if (restoringTemporaryState)
		return;
	TemporaryProcessingMutexGuard processingLock;
	if (!processingLock.isLocked())
	{
		QSignalBlocker blocker(bypassAction);
		bypassAction->setChecked(!bypassTable.isNull());
		showWorkspaceStatus(
			tr("A temporary audio state could not be recovered automatically"),
			"danger", 0);
		return;
	}
	FilterTable* filterTable = currentFilterTable();
	if (enabled)
	{
		if (filterTable == NULL || filterTable->getConfigPath().isEmpty())
		{
			QSignalBlocker blocker(bypassAction);
			bypassAction->setChecked(false);
			showWorkspaceStatus(tr("Save the profile before using audible bypass"), "warning");
			return;
		}
		// Validate external rows before deriving the original/bypass lines and
		// their recovery journal. setTemporaryLines commits them only after its
		// conditional write.
		if (!filterTable->prepareDeleteAllItems() ||
			isFilterTableDirty(filterTable))
		{
			QSignalBlocker blocker(bypassAction);
			bypassAction->setChecked(false);
			showWorkspaceStatus(tr("Save the profile before using audible bypass"), "warning");
			return;
		}
		if (comparisonAction != NULL && comparisonAction->isChecked())
		{
			comparisonAction->setChecked(false);
			if (comparisonAction->isChecked())
			{
				QSignalBlocker blocker(bypassAction);
				bypassAction->setChecked(false);
				return;
			}
		}
		recoverInterruptedTemporaryProcessingState();
		if (hasConflictingTemporaryJournal(
			filterTable->getConfigPath(), temporaryRecoveryOwner))
		{
			QSignalBlocker blocker(bypassAction);
			bypassAction->setChecked(false);
			showWorkspaceStatus(
				tr("A temporary audio state could not be recovered automatically"),
				"danger", 0);
			return;
		}

		bypassTable = filterTable;
		bypassOriginalLines = filterTable->getLines();
		bypassTemporaryLines.clear();
		for (const QString& line : bypassOriginalLines)
		{
			const QString trimmed = line.trimmed();
			bypassTemporaryLines.append(trimmed.isEmpty() || trimmed.startsWith('#')
				? line : QStringLiteral("# [Temporary bypass] ") + line);
		}
		bool externalConflict = false;
		if (!prepareTemporaryContents(
			filterTable, bypassOriginalLines, bypassTemporaryLines,
			&bypassOriginalContent, &bypassTemporaryContent,
			&externalConflict))
		{
			bypassTable.clear();
			bypassOriginalLines.clear();
			bypassTemporaryLines.clear();
			bypassOriginalContent.clear();
			bypassTemporaryContent.clear();
			QSignalBlocker blocker(bypassAction);
			bypassAction->setChecked(false);
			showWorkspaceStatus(
				externalConflict
					? tr("Kept external profile changes")
					: tr("Could not activate temporary bypass"),
				externalConflict ? "warning" : "danger",
				externalConflict ? 4500 : 0);
			return;
		}
		if (!writeTemporaryRecoveryJournal(
			filterTable, bypassOriginalLines, bypassOriginalContent,
			bypassTemporaryContent, QStringLiteral("bypass")))
		{
			bypassTable.clear();
			bypassOriginalLines.clear();
			bypassTemporaryLines.clear();
			bypassOriginalContent.clear();
			bypassTemporaryContent.clear();
			QSignalBlocker blocker(bypassAction);
			bypassAction->setChecked(false);
			showWorkspaceStatus(tr("Could not activate temporary bypass"), "danger", 0);
			return;
		}
		if (!setTemporaryLines(
			filterTable, bypassTemporaryLines, bypassOriginalContent,
			bypassTemporaryContent, &externalConflict))
		{
			const bool restored = externalConflict || restoreTemporaryLines(
				filterTable, bypassOriginalLines, bypassOriginalContent,
				bypassTemporaryContent);
			if (!restored)
			{
				filterTable->setEnabled(false);
				bypassAction->setText(tr("Restore audio"));
				QSignalBlocker blocker(bypassAction);
				bypassAction->setChecked(true);
				showWorkspaceStatus(
					tr("A temporary audio state could not be recovered automatically"),
					"danger", 0);
				refreshWorkspaceActionState();
				return;
			}
			clearTemporaryRecoveryJournal();
			filterTable->setEnabled(true);
			bypassTable.clear();
			bypassOriginalLines.clear();
			bypassTemporaryLines.clear();
			bypassOriginalContent.clear();
			bypassTemporaryContent.clear();
			QSignalBlocker blocker(bypassAction);
			bypassAction->setChecked(false);
			showWorkspaceStatus(
				externalConflict
					? tr("Kept external profile changes")
					: tr("Could not activate temporary bypass"),
				externalConflict ? "warning" : "danger",
				externalConflict ? 4500 : 0);
			return;
		}
		filterTable->setEnabled(false);
		bypassAction->setText(tr("Restore audio"));
		showWorkspaceStatus(tr("Current profile is temporarily bypassed"), "danger", 0);
		if (trayIcon != NULL)
			trayIcon->showMessage(
				tr("Hibiki EQAPO bypassed"),
				tr("Choose Restore audio to re-enable the current profile."),
				QSystemTrayIcon::Warning, 3500);
	}
	else
	{
		bool restored = true;
		if (!bypassTable.isNull())
		{
			restored = restoreTemporaryLines(
				bypassTable, bypassOriginalLines, bypassOriginalContent,
				bypassTemporaryContent);
			if (restored)
				clearTemporaryRecoveryJournal();
			else
			{
				QSignalBlocker blocker(bypassAction);
				bypassAction->setChecked(true);
				bypassAction->setText(tr("Restore audio"));
				showWorkspaceStatus(
					tr("A temporary audio state could not be recovered automatically"),
					"danger", 0);
				refreshWorkspaceActionState();
				return;
			}
			bypassTable->setEnabled(true);
		}
		bypassTable.clear();
		bypassOriginalLines.clear();
		bypassTemporaryLines.clear();
		bypassOriginalContent.clear();
		bypassTemporaryContent.clear();
		bypassAction->setText(tr("Bypass"));
		showWorkspaceStatus(tr("Audio processing restored"));
	}
	refreshWorkspaceActionState();
}

bool MainWindow::restoreTemporaryProcessingState()
{
	if (restoringTemporaryState)
		return true;
	if (!temporaryFilterTable.isNull()
		&& !restoreTemporaryFilterConfiguration())
		return false;
	TemporaryProcessingMutexGuard processingLock;
	if (!processingLock.isLocked())
	{
		showWorkspaceStatus(
			tr("A temporary audio state could not be recovered automatically"),
			"danger", 0);
		return false;
	}
	QScopedValueRollback<bool> restoringGuard(restoringTemporaryState, true);
	const bool hadTemporaryState =
		(showingComparisonA && !comparisonTable.isNull()) || !bypassTable.isNull();
	if (showingComparisonA && !comparisonTable.isNull())
	{
		if (!restoreTemporaryLines(
			comparisonTable, comparisonBLines, comparisonOriginalContent,
			comparisonTemporaryContent))
			return false;
		comparisonTable->setEnabled(true);
	}
	showingComparisonA = false;
	comparisonBLines.clear();
	comparisonOriginalContent.clear();
	comparisonTemporaryContent.clear();
	if (comparisonAction != NULL)
	{
		QSignalBlocker blocker(comparisonAction);
		comparisonAction->setChecked(false);
		comparisonAction->setText(tr("Compare A"));
	}
	if (!bypassTable.isNull())
	{
		if (!restoreTemporaryLines(
			bypassTable, bypassOriginalLines, bypassOriginalContent,
			bypassTemporaryContent))
			return false;
		bypassTable->setEnabled(true);
	}
	bypassTable.clear();
	bypassOriginalLines.clear();
	bypassTemporaryLines.clear();
	bypassOriginalContent.clear();
	bypassTemporaryContent.clear();
	if (bypassAction != NULL)
	{
		QSignalBlocker blocker(bypassAction);
		bypassAction->setChecked(false);
		bypassAction->setText(tr("Bypass"));
	}
	if (hadTemporaryState)
		clearTemporaryRecoveryJournal();
	refreshWorkspaceActionState();
	return true;
}

void MainWindow::refreshWorkspaceActionState()
{
	FilterTable* filterTable = currentFilterTable();
	const bool hasTable = filterTable != NULL;
	const bool hasSavedProfile = hasTable && !filterTable->getConfigPath().isEmpty();
	const bool hasTemporaryState = showingComparisonA || !bypassTable.isNull();
	if (doublePrecisionAction)
	{
		QSignalBlocker blocker(doublePrecisionAction);
		QSignalBlocker checkBoxBlocker(doublePrecisionCheckBox);
		bool doublePrecision = true;
		int directives = 0;
		bool validPrecision = true;
		if (filterTable)
			for (const QString& line : filterTable->getLines())
			{
				const QString text = line.trimmed();
				if (text.section(':', 0, 0).trimmed() != QStringLiteral("ProcessingPrecision")) continue;
				++directives;
				const QString value = text.mid(text.indexOf(':') + 1).trimmed();
				validPrecision = validPrecision && (value == "32" || value == "64");
				doublePrecision = value != "32";
			}
		const bool unambiguous = validPrecision && directives <= 1;
		const bool editable = hasTable && unambiguous && filterTable->processingPrecisionEditable();
		doublePrecisionAction->setCheckable(true);
		doublePrecisionAction->setChecked(doublePrecision);
		doublePrecisionAction->setText(hasTable && !editable
			? tr("Processing precision (see configuration)")
			: tr("Double precision (64-bit signal path)"));
		doublePrecisionAction->setEnabled(editable && !hasTemporaryState && validPrecision && directives <= 1);
		if (doublePrecisionCheckBox != NULL)
		{
			doublePrecisionCheckBox->setChecked(doublePrecision);
			doublePrecisionCheckBox->setEnabled(
				editable && !hasTemporaryState && validPrecision && directives <= 1);
			doublePrecisionCheckBox->setText(tr("Double precision"));
		}
		if (hasTable && !unambiguous)
			doublePrecisionAction->setToolTip(tr("Could not change precision. Check for duplicate or invalid ProcessingPrecision entries."));
		else if (hasTable && !editable)
			doublePrecisionAction->setToolTip(tr("This configuration contains scopes or included files. Edit ProcessingPrecision in the configuration text to choose the effective signal-path format."));
		else
			doublePrecisionAction->setToolTip(tr("Applies to the current configuration. Off uses 32-bit audio between modules; specialized filters may retain double-precision internals. Save to apply when instant mode is off."));
		if (doublePrecisionCheckBox != NULL)
			doublePrecisionCheckBox->setToolTip(doublePrecisionAction->toolTip());
	}
	ui->actionSave->setEnabled(hasTable && !hasTemporaryState);
	ui->actionSaveAs->setEnabled(hasTable && !hasTemporaryState);
	if (captureComparisonAction != NULL)
		captureComparisonAction->setEnabled(hasSavedProfile && bypassTable.isNull());
	if (comparisonAction != NULL)
		comparisonAction->setEnabled(
			hasSavedProfile && !comparisonTable.isNull() && comparisonTable == filterTable && bypassTable.isNull());
	if (bypassAction != NULL)
		bypassAction->setEnabled(hasSavedProfile && (!showingComparisonA || bypassAction->isChecked()));
	if (searchLineEdit != NULL)
		searchLineEdit->setEnabled(hasTable);
	refreshAutoPreampActionState();
}

void MainWindow::invalidateAnalysisResult()
{
	latestAnalysisResultValid = false;
	acceptedAnalysisGeneration = 0;
	latestAnalysisPeakGain = 0.0;
	latestAnalysisConfigurationFiles.clear();
	latestAnalysisVolumeSnapshots.clear();
	if (autoPreampButton != NULL)
		autoPreampButton->setEnabled(false);
}

bool MainWindow::analysisResultCanAdjustPreamp() const
{
	if (!latestAnalysisResultValid || acceptedAnalysisGeneration == 0
		|| acceptedAnalysisGeneration != requestedAnalysisGeneration
		|| !std::isfinite(latestAnalysisPeakGain) || latestAnalysisPeakGain <= 0.0
		|| showingComparisonA || !bypassTable.isNull() || restoringTemporaryState)
		return false;

	FilterTable* filterTable = currentFilterTable();
	if (filterTable == NULL || filterTable != requestedAnalysisTable
		|| filterTable->getConfigPath().isEmpty()
		|| configurationPathKey(filterTable->getConfigPath())
			!= configurationPathKey(requestedAnalysisConfigPath)
		|| isFilterTableDirty(filterTable)
		|| requestedAnalysisStartFrom != ui->startFromComboBox->currentIndex()
		|| requestedAnalysisChannelIndex != ui->analysisChannelComboBox->currentIndex())
		return false;

	shared_ptr<AbstractAPOInfo> selectedDevice;
	int channelMask = 0;
	getDeviceAndChannelMask(&selectedDevice, &channelMask);
	if (channelMask != requestedAnalysisChannelMask
		|| analysisDeviceId(selectedDevice).compare(
			requestedAnalysisDeviceId, Qt::CaseInsensitive) != 0)
		return false;
	if (!analysisFilesStillMatch(latestAnalysisConfigurationFiles)
		|| !analysisRootMatchesEditor(
			latestAnalysisConfigurationFiles,
			requestedAnalysisConfigPath,
			filterTable->getLines())
		|| !analysisVolumesStillMatch(latestAnalysisVolumeSnapshots)
		|| !analysisTopologySupportsAutoPreamp(latestAnalysisConfigurationFiles))
		return false;

	const QByteArray currentHash = QCryptographicHash::hash(
		serializeConfigurationLines(filterTable->getLines()),
		QCryptographicHash::Sha256);
	if (currentHash != requestedAnalysisLinesHash)
		return false;

	FilterTable::PreampAdjustmentPlan plan;
	return filterTable->planPreampReduction(
		conservativePreampReduction(latestAnalysisPeakGain), &plan);
}

void MainWindow::refreshAutoPreampActionState()
{
	if (autoPreampButton != NULL)
		autoPreampButton->setEnabled(analysisResultCanAdjustPreamp());
}

void MainWindow::closeToTrayToggled(bool enabled)
{
	closeToTray = enabled;
	if (enabled && trayIcon == NULL)
	{
		closeToTray = false;
		QSignalBlocker blocker(closeToTrayAction);
		closeToTrayAction->setChecked(false);
		showWorkspaceStatus(tr("The Windows notification area is unavailable"), "warning");
	}
}

void MainWindow::takeoverVolumeKeysToggled(bool enabled)
{
	takeoverVolumeKeys = enabled;
	VolumeTakeoverManager::instance()->setTakeoverEnabled(enabled);
	if (enabled)
	{
		showWorkspaceStatus(tr("Volume takeover active: volume keys control Hibiki EQAPO loudness with OSD"));
		VolumeTakeoverManager::instance()->showCurrentVolumeOsd();
	}
	else
	{
		showWorkspaceStatus(tr("Volume takeover disabled: Windows default volume behavior restored"));
	}
}

void MainWindow::doChecks()
{
	if (!DeviceAPOInfo::checkProtectedAudioDG(false) || !DeviceAPOInfo::checkAPORegistration(false))
	{
		if (QMessageBox::warning(this, tr("Registry problem"), tr("A registry value that is required for the operation of Hibiki EQAPO is not set correctly.\nDo you want to run the Device Selector application to fix the problem?"), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
		{
			runDeviceSelector();
			return;
		}
	}

	if (!hasInstalledDevice(outputDevices) && !hasInstalledDevice(inputDevices))
	{
		if (QMessageBox::warning(this, tr("APO not installed to device"), tr("Hibiki EQAPO has not been installed to the selected device.\nDo you want to run the Device Selector application to fix the problem?"), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
		{
			runDeviceSelector();
			return;
		}
	}

	AbstractAPOInfo* disabledApoInfo = NULL;
	for (shared_ptr<AbstractAPOInfo>& apoInfo : outputDevices)
	{
		if (apoInfo->isInstalled() && apoInfo->isEnhancementsDisabled())
		{
			disabledApoInfo = apoInfo.get();
			break;
		}
	}

	if (disabledApoInfo == NULL)
	{
		for (shared_ptr<AbstractAPOInfo>& apoInfo : inputDevices)
		{
			if (apoInfo->isInstalled() && apoInfo->isEnhancementsDisabled())
			{
				disabledApoInfo = apoInfo.get();
				break;
			}
		}
	}

	if (disabledApoInfo != NULL)
	{
		if (QMessageBox::warning(this, tr("Audio enhancements disabled"), tr("Audio enhancements are not enabled for the device\n%1 %2.\nDo you want to run the Device Selector application to fix the problem?").arg(QString::fromStdWString(disabledApoInfo->getConnectionName())).arg(QString::fromStdWString(disabledApoInfo->getDeviceName())), QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
		{
			runDeviceSelector();
			return;
		}
	}
}

void MainWindow::runDeviceSelector()
{
	// cannot use QProcess::startDetached because of UAC
	wstring file = (QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/DeviceSelector.exe")).toStdWString();
	unsigned long long result = (unsigned long long)ShellExecuteW(NULL, L"open", file.c_str(), NULL, NULL, SW_SHOWNORMAL);
	if (result == SE_ERR_ACCESSDENIED)
		result = (unsigned long long)ShellExecuteW(NULL, L"runas", file.c_str(), NULL, NULL, SW_SHOWNORMAL);
	if (result <= 32)
		QMessageBox::warning(this, tr("Device Selector"),
			tr("Could not open Device Selector. Check that DeviceSelector.exe is installed beside the editor."));
}

bool MainWindow::load(QString path)
{
	path = QDir::toNativeSeparators(path);

	for (int i = 0; i < ui->tabWidget->count(); i++)
	{
		QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(i));
		FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());

		if (QDir::cleanPath(filterTable->getConfigPath()).compare(
			QDir::cleanPath(path), Qt::CaseInsensitive) == 0)
		{
			ui->tabWidget->setCurrentIndex(i);
			return true;
		}
	}

	QElapsedTimer timer;
	timer.start();

	HANDLE hFile = INVALID_HANDLE_VALUE;
	while (hFile == INVALID_HANDLE_VALUE)
	{
		hFile = CreateFile(path.toStdWString().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			if (error != ERROR_SHARING_VIOLATION)
			{
				QMessageBox::critical(this, tr("Error"), tr("Error while reading configuration file: %1").arg(QString::fromStdWString(StringHelper::getSystemErrorString(error))));
				return false;
			}

			// file is being written, so wait
			Sleep(1);
		}
	}

	stringstream inputStream;

	char buf[8192];
	unsigned long bytesRead = -1;
	while (ReadFile(hFile, buf, sizeof(buf), &bytesRead, NULL) && bytesRead != 0)
	{
		inputStream.write(buf, bytesRead);
	}

	CloseHandle(hFile);

	inputStream.seekg(0);

	QList<QString> lines;
	while (inputStream.good())
	{
		string encodedLine;
		getline(inputStream, encodedLine);
		if (encodedLine.size() > 0 && encodedLine[encodedLine.size() - 1] == '\r')
			encodedLine.resize(encodedLine.size() - 1);

		wstring line = StringHelper::toWString(encodedLine, CP_UTF8);
		if (line.find(L'\uFFFD') != wstring::npos)
			line = StringHelper::toWString(encodedLine, CP_ACP);

		lines.append(QString::fromStdWString(line));
	}

	QFileInfo fileInfo(path);
	FilterTable* filterTable = addTab(fileInfo.fileName(), QDir::toNativeSeparators(fileInfo.absoluteFilePath()), path, lines);

	connect(filterTable, SIGNAL(linesChanged()), this, SLOT(linesChanged()));

	qDebug("Loading took %.1f ms", timer.nsecsElapsed() / 1e6);

	ui->tabWidget->setCurrentIndex(ui->tabWidget->count() - 1);

	recentFiles.removeAll(path);
	recentFiles.prepend(path);
	if (recentFiles.size() > 10)
		recentFiles.removeLast();
	updateRecentFiles();
	refreshProfiles();
	syncProfileSelection();
	refreshWorkspaceActionState();
	return true;
}

bool MainWindow::save(FilterTable* filterTable, QString path)
{
	QElapsedTimer timer;
	timer.start();

	const QByteArray byteArray = serializeConfigurationLines(filterTable->getLines());

	HANDLE hFile = INVALID_HANDLE_VALUE;
	while (hFile == INVALID_HANDLE_VALUE)
	{
		hFile = CreateFile(path.toStdWString().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			if (error != ERROR_SHARING_VIOLATION)
			{
				QMessageBox::critical(this, tr("Error"), tr("Error while writing configuration file: %1").arg(QString::fromStdWString(StringHelper::getSystemErrorString(error))));
				return false;
			}

			// file is being written, so wait
			Sleep(1);
		}
	}

	unsigned long bytesWritten = 0;
	const BOOL writeSucceeded = WriteFile(
		hFile, byteArray.constData(), byteArray.length(), &bytesWritten, NULL);
	if (!writeSucceeded || bytesWritten != byteArray.length())
	{
		// should never happen
		QMessageBox::critical(this, tr("Error"), tr("Only %1/%2 bytes have been written!").arg(bytesWritten).arg(byteArray.length()));
	}

	CloseHandle(hFile);

	qDebug("Saving took %.1f ms", timer.nsecsElapsed() / 1e6);

	startAnalysis();
	refreshProfiles();
	return writeSucceeded && bytesWritten == byteArray.length();
}

bool MainWindow::isEmpty()
{
	return ui->tabWidget->count() == 0;
}

bool MainWindow::shouldRestart()
{
	return restart;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	if (closeToTray && !quitRequested && !restart && trayIcon != NULL && trayIcon->isVisible())
	{
		savePreferences();
		hide();
		event->ignore();
		trayIcon->showMessage(
			tr("Hibiki EQAPO is still running"),
			tr("Use the notification-area icon to reopen profiles or restore bypassed audio."),
			QSystemTrayIcon::Information, 3000);
		return;
	}

	bool canceled = false;
	for (int i = 0; i < ui->tabWidget->count(); ++i)
	{
		QScrollArea* scrollArea = qobject_cast<QScrollArea*>(
			ui->tabWidget->widget(i));
		FilterTable* filterTable = scrollArea == NULL
			? NULL : qobject_cast<FilterTable*>(scrollArea->widget());
		if (filterTable != NULL && !filterTable->prepareDeleteAllItems())
		{
			ui->tabWidget->setCurrentIndex(i);
			canceled = true;
			break;
		}
	}
	if (!canceled)
	{
		for (int i = 0; i < ui->tabWidget->count(); i++)
		{
			if (!askForClose(i))
			{
				canceled = true;
				break;
			}
		}
	}
	// A close prompt can still be canceled, so keep temporary audio and every
	// external host untouched until all tabs have accepted the close.
	if (!canceled && !restoreTemporaryProcessingState())
		canceled = true;
	if (!canceled)
	{
		for (int i = 0; i < ui->tabWidget->count(); ++i)
		{
			QScrollArea* scrollArea = qobject_cast<QScrollArea*>(
				ui->tabWidget->widget(i));
			FilterTable* filterTable = scrollArea == NULL
				? NULL : qobject_cast<FilterTable*>(scrollArea->widget());
			if (filterTable != NULL && !filterTable->commitDeleteAllItems())
			{
				ui->tabWidget->setCurrentIndex(i);
				canceled = true;
				break;
			}
		}
	}

	if (canceled)
	{
		event->ignore();
		restart = false;
		quitRequested = false;
		noSavePreferences = false;
		noSaveFilePreferences = false;
	}
	else
	{
		savePreferences();
	}
}

void MainWindow::deviceSelected(int index)
{
	shared_ptr<AbstractAPOInfo> apoInfo = deviceComboBox->itemData(index).value<shared_ptr<AbstractAPOInfo>>();
	if (apoInfo == NULL)
		apoInfo = defaultOutputDevice;

	channelConfigurationComboBox->clear();

	const QList<GUIChannelHelper::ChannelConfigurationInfo>& infos = GUIChannelHelper::getInstance()->getChannelConfigurationInfos();

	if (apoInfo != NULL)
	{
		const GUIChannelHelper::ChannelConfigurationInfo* selectedInfo = NULL;
		for (const GUIChannelHelper::ChannelConfigurationInfo& info : infos)
		{
			if (info.channelMask == (int)apoInfo->getChannelMask())
			{
				selectedInfo = &info;
				break;
			}
		}

		if (selectedInfo != NULL)
			channelConfigurationComboBox->addItem(tr("From device") + " (" + selectedInfo->name + ")", 0);
		else if (apoInfo->getChannelCount() != 0)
			channelConfigurationComboBox->addItem((tr("From device") + " (%1 channels)").arg(apoInfo->getChannelCount()), 0);
		else
			channelConfigurationComboBox->addItem(tr("From device") + " (? channels)", 0);
	}
	else
	{
		channelConfigurationComboBox->addItem(tr("From device") + " (?)", 0);
	}

	for (const GUIChannelHelper::ChannelConfigurationInfo& info : infos)
		channelConfigurationComboBox->addItem(info.name, info.channelMask);

	channelConfigurationSelected(channelConfigurationComboBox->currentIndex());

	if (sender() == deviceComboBox)
	{
		const QString linkedPath = linkedProfileForCurrentDevice();
		if (!linkedPath.isEmpty() && QFileInfo::exists(linkedPath) &&
			QDir::cleanPath(linkedPath).compare(currentProfilePath(), Qt::CaseInsensitive) != 0)
		{
			if (!restoreTemporaryProcessingState())
				return;
			if (load(linkedPath))
				showWorkspaceStatus(tr("Opened the profile linked to this device: %1")
					.arg(QFileInfo(linkedPath).completeBaseName()));
		}
	}
}

void MainWindow::channelConfigurationSelected(int index)
{
	shared_ptr<AbstractAPOInfo> selectedDevice;
	int channelMask;
	getDeviceAndChannelMask(&selectedDevice, &channelMask);

	for (int i = 0; i < ui->tabWidget->count(); i++)
	{
		QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(i));
		FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
		filterTable->updateDeviceAndChannelMask(selectedDevice, channelMask);
	}

	ui->analysisChannelComboBox->clear();

	if (selectedDevice != NULL)
	{
		unsigned channelCount = selectedDevice->getChannelCount();
		if (channelMask != 0 && channelMask != (int)selectedDevice->getChannelMask())
		{
			channelCount = 0;
			for (int i = 0; i < 31; i++)
			{
				int channelPos = 1 << i;
				if (channelMask & channelPos)
					channelCount++;
			}
		}
		if (channelCount == 0)
		{
			channelCount = 8;
			channelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
		}

		vector<wstring> channelNames = ChannelHelper::getChannelNames(channelCount, channelMask);
		for (const wstring& channelName : channelNames)
		{
			ui->analysisChannelComboBox->addItem(QString::fromStdWString(channelName));
		}
	}

	startAnalysis();
}

void MainWindow::linesChanged()
{
	if (restoringTemporaryState)
		return;
	invalidateAnalysisResult();

	FilterTable* filterTable = qobject_cast<FilterTable*>(sender());
	// A modal calibration writes a crash-recoverable runtime-only contour
	// bypass. Timers may still emit model changes while the dialog is open;
	// never let those overwrite the temporary file or its recovery contract.
	if (!temporaryFilterTable.isNull() && filterTable == temporaryFilterTable)
		return;
	bool savedInstantly = false;

	if (instantModeCheckBox->isChecked() && !suppressInstantSave &&
		!applyingAutoPreampAdjustment)
	{
		QString configPath = filterTable->getConfigPath();
		if (configPath.length() > 0)
			savedInstantly = save(filterTable, configPath);
	}

	int tabIndex = -1;
	for (int i = 0; i < ui->tabWidget->count(); i++)
	{
		QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(i));
		if (scrollArea->widget() == filterTable)
		{
			tabIndex = i;
			break;
		}
	}
	if (tabIndex < 0)
		return;
	QString tabText = ui->tabWidget->tabText(tabIndex);
	if (savedInstantly)
	{
		if (tabText.endsWith('*'))
			ui->tabWidget->setTabText(tabIndex, tabText.left(tabText.length() - 1));
	}
	else if (!tabText.endsWith('*'))
	{
		tabText += '*';
		ui->tabWidget->setTabText(tabIndex, tabText);
	}
	// The header precision switch is always visible, unlike the Settings menu
	// action which refreshes when the menu opens. Keep it in sync with text
	// edits such as a hand-typed ProcessingPrecision or Include line.
	if (filterTable == currentFilterTable())
		refreshWorkspaceActionState();
}

bool MainWindow::on_tabWidget_tabCloseRequested(int index)
{
	QScrollArea* closingScrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(index));
	FilterTable* closingFilterTable = closingScrollArea == NULL
		? NULL : qobject_cast<FilterTable*>(closingScrollArea->widget());
	if (closingFilterTable != NULL &&
		!closingFilterTable->prepareDeleteAllItems())
		return false;

	if (!askForClose(index))
		return false;
	// The user has now accepted the close. Restoring temporary processing may
	// commit external row cleanup, so it must not run before the Cancel point.
	if ((closingFilterTable == comparisonTable ||
		closingFilterTable == bypassTable) &&
		!restoreTemporaryProcessingState())
	{
		return false;
	}
	if (closingFilterTable != NULL &&
		!closingFilterTable->commitDeleteAllItems())
		return false;

	if (closingFilterTable != NULL)
	{
		QString path = closingFilterTable->getConfigPath();
		recentFiles.removeAll(path);
		recentFiles.prepend(path);
		if (recentFiles.size() > 10)
			recentFiles.removeLast();
		updateRecentFiles();
	}

	ui->tabWidget->removeTab(index);
	if (closingFilterTable == comparisonTable)
	{
		comparisonTable.clear();
		comparisonALines.clear();
		comparisonBLines.clear();
		comparisonOriginalContent.clear();
		comparisonTemporaryContent.clear();
	}
	refreshProfiles();
	syncProfileSelection();
	refreshWorkspaceActionState();
	// QTabWidget::removeTab() only detaches the page. Release the scroll
	// area (and its FilterTable) after every pointer/state consumer above
	// has finished; a veto or canceled close never reaches this point.
	if (closingScrollArea != NULL)
		closingScrollArea->deleteLater();
	return true;
}

void MainWindow::on_actionOpen_triggered()
{
	if (!restoreTemporaryProcessingState())
		return;
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	QString path;
	if (scrollArea != NULL)
	{
		FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
		if (filterTable->getConfigPath().length() > 0)
		{
			QFileInfo fileInfo(filterTable->getConfigPath());
			path = fileInfo.absolutePath();
		}
	}
	if (path.length() == 0)
		path = configDir.absolutePath();

	QFileDialog dialog(this, tr("Open file"), path, "*.txt");
	dialog.setFileMode(QFileDialog::ExistingFiles);
	dialog.setNameFilter(tr("E-APO configurations (*.txt)"));

	if (dialog.exec() == QDialog::Accepted)
	{
		for (QString file : dialog.selectedFiles())
			load(file);
	}
}

void MainWindow::on_actionSave_triggered()
{
	if (!restoreTemporaryProcessingState())
		return;
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	if (scrollArea == NULL)
		return;

	FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());

	if (filterTable->getConfigPath().length() == 0)
	{
		ui->actionSaveAs->trigger();
	}
	else
	{
		if (!save(filterTable, filterTable->getConfigPath()))
			return;

		QString tabText = ui->tabWidget->tabText(ui->tabWidget->currentIndex());
		if (tabText.endsWith('*'))
			ui->tabWidget->setTabText(ui->tabWidget->currentIndex(), tabText.left(tabText.length() - 1));
	}
}

void MainWindow::on_actionSaveAs_triggered()
{
	if (!restoreTemporaryProcessingState())
		return;
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	if (scrollArea == NULL)
		return;

	FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
	QString path;
	QString filename;
	if (filterTable->getConfigPath().length() == 0)
	{
		path = configDir.absolutePath();
	}
	else
	{
		QFileInfo fileInfo(filterTable->getConfigPath());
		path = fileInfo.absolutePath();
		filename = fileInfo.fileName();
	}
	QFileDialog dialog(this, tr("Save file as"), path, "*.txt");
	dialog.setFileMode(QFileDialog::AnyFile);
	dialog.setAcceptMode(QFileDialog::AcceptSave);
	dialog.setNameFilter(tr("E-APO configurations (*.txt)"));
	dialog.setDefaultSuffix(".txt");
	if (filename.length() > 0)
		dialog.selectFile(filename);

	if (dialog.exec() == QDialog::Accepted)
	{
		QString savePath = dialog.selectedFiles().at(0);
		if (!save(filterTable, savePath))
			return;
		filterTable->setConfigPath(QDir::toNativeSeparators(savePath));
		// save() queued analysis while the table still had its old path. Queue a
		// new generation for the path that Save As actually established.
		startAnalysis();

		QFileInfo fileInfo(savePath);
		ui->tabWidget->setTabText(ui->tabWidget->currentIndex(), fileInfo.fileName());
		ui->tabWidget->setTabToolTip(ui->tabWidget->currentIndex(), QDir::toNativeSeparators(fileInfo.absoluteFilePath()));
		refreshProfiles();
		syncProfileSelection();
		refreshWorkspaceActionState();
	}
}

void MainWindow::on_actionNew_triggered()
{
	if (!restoreTemporaryProcessingState())
		return;
	FilterTable* filterTable = addTab(tr("Unsaved"), "", "", QList<QString>());

	connect(filterTable, SIGNAL(linesChanged()), this, SLOT(linesChanged()));
	ui->tabWidget->setCurrentIndex(ui->tabWidget->count() - 1);
	syncProfileSelection();
	refreshWorkspaceActionState();
}

void MainWindow::recentFileSelected()
{
	if (!restoreTemporaryProcessingState())
		return;
	QAction* action = qobject_cast<QAction*>(sender());
	load(action->text());
}

void MainWindow::on_actionCut_triggered()
{
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	if (scrollArea == NULL)
		return;

	FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
	filterTable->cut();
}

void MainWindow::on_actionCopy_triggered()
{
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	if (scrollArea == NULL)
		return;

	FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
	filterTable->copy();
}

void MainWindow::on_actionPaste_triggered()
{
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	if (scrollArea == NULL)
		return;

	FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
	filterTable->paste();
}

void MainWindow::on_actionDelete_triggered()
{
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	if (scrollArea == NULL)
		return;

	FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
	filterTable->deleteSelectedLines();
}

void MainWindow::on_actionSelectAll_triggered()
{
	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
	if (scrollArea == NULL)
		return;

	FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
	filterTable->selectAll();
}

void MainWindow::instantModeEnabled(bool enabled)
{
	if (enabled)
	{
		if (!restoreTemporaryProcessingState())
		{
			instantModeCheckBox->setChecked(false);
			return;
		}
		for (int i = 0; i < ui->tabWidget->count(); i++)
		{
			QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(i));
			FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());

			if (filterTable->getConfigPath().length() > 0)
			{
				if (!save(filterTable, filterTable->getConfigPath()))
					continue;

				QString tabText = ui->tabWidget->tabText(i);
				if (tabText.endsWith('*'))
					ui->tabWidget->setTabText(i, tabText.left(tabText.length() - 1));
			}
		}
	}
}

void MainWindow::on_tabWidget_currentChanged(int index)
{
	Q_UNUSED(index);
	if (!restoreTemporaryProcessingState())
	{
		FilterTable* temporaryTable = showingComparisonA
			? comparisonTable.data() : bypassTable.data();
		for (int tabIndex = 0; temporaryTable != NULL
			&& tabIndex < ui->tabWidget->count(); ++tabIndex)
		{
			QScrollArea* scrollArea = qobject_cast<QScrollArea*>(
				ui->tabWidget->widget(tabIndex));
			if (scrollArea != NULL && scrollArea->widget() == temporaryTable)
			{
				QSignalBlocker blocker(ui->tabWidget);
				ui->tabWidget->setCurrentIndex(tabIndex);
				break;
			}
		}
		syncProfileSelection();
		refreshWorkspaceActionState();
		return;
	}
	syncProfileSelection();
	refreshWorkspaceActionState();
	if (searchLineEdit != NULL && !searchLineEdit->text().trimmed().isEmpty())
		findNext();
	startAnalysis();
}

void MainWindow::on_startFromComboBox_activated(int index)
{
	startAnalysis();
}

void MainWindow::on_analysisChannelComboBox_activated(int index)
{
	startAnalysis();
}

void MainWindow::on_resolutionSpinBox_valueChanged(int value)
{
	startAnalysis();
}

void MainWindow::updateAnalysisPanel()
{
	analysisThread->beginGetResult();
	const quint64 resultGeneration = analysisThread->getResultGeneration();
	if (resultGeneration == 0 || resultGeneration != requestedAnalysisGeneration)
	{
		// A newer device/channel/file request is already pending. Do not briefly
		// publish the older response or enable a destructive action from it.
		analysisThread->endGetResult();
		return;
	}
	int sampleRate = analysisThread->getFreqDataSampleRate();
	int latency = analysisThread->getLatency();
	if (sampleRate <= 0)
	{
		invalidateAnalysisResult();
		analysisThread->endGetResult();
		ui->peakGainValueLabel->setText(QStringLiteral("—"));
		ui->latencyValueLabel->setText(QStringLiteral("—"));
		ui->initTimeValueLabel->setText(QStringLiteral("—"));
		ui->cpuUsageValueLabel->setText(QStringLiteral("—"));
		setAnalysisStatus(
			analysisStateLabel,
			tr("Analysis is unavailable for the selected device"),
			"warning",
			false);
		if (headroomValueLabel != NULL)
		{
			headroomValueLabel->setText(QStringLiteral("—"));
			setStatusLevel(headroomValueLabel, "warning");
		}
		return;
	}
	analysisPlotScene->setFreqData(analysisThread->getFreqData(), analysisThread->getFreqDataLength(), sampleRate);

	double peakGain = analysisThread->getPeakGain();
	latestAnalysisPeakGain = peakGain;
	acceptedAnalysisGeneration = resultGeneration;
	latestAnalysisConfigurationFiles = analysisThread->getConfigurationFiles();
	latestAnalysisVolumeSnapshots = analysisThread->getVolumeSnapshots();
	latestAnalysisResultValid = std::isfinite(peakGain);
	if (std::isfinite(peakGain))
	{
		ui->peakGainValueLabel->setText(tr("%1 dB").arg(peakGain, 0, 'f', 1));
		setStatusLevel(ui->peakGainValueLabel, peakGain > 0 ? "danger" : "normal");
		const double headroom = (std::max)(0.0, -peakGain);
		if (headroomValueLabel != NULL)
		{
			headroomValueLabel->setText(tr("%1 dB").arg(headroom, 0, 'f', 1));
			setStatusLevel(headroomValueLabel, peakGain > 0 ? "danger" : "normal");
		}
	}
	else
	{
		ui->peakGainValueLabel->setText(QStringLiteral("—"));
		if (headroomValueLabel != NULL)
			headroomValueLabel->setText(QStringLiteral("—"));
	}

	ui->latencyValueLabel->setText(tr("%1 ms (%2 s.)").arg(latency * 1000.0 / sampleRate, 0, 'f', 1).arg(latency));

	ui->initTimeValueLabel->setText(tr("%1 ms").arg(analysisThread->getInitializationTime(), 0, 'f', 1));

	const double processedFrames = analysisThread->getProcessedFrames();
	if (processedFrames > 0)
	{
		double cpuUsage = analysisThread->getProcessingTime() * 100.0 /
			(processedFrames * 1000.0 / sampleRate);
		ui->cpuUsageValueLabel->setText(tr("%1 % (one core)").arg(cpuUsage, 0, 'f', 1));
		setStatusLevel(
			ui->cpuUsageValueLabel,
			cpuUsage >= 50 ? "danger" : (cpuUsage >= 20 ? "warning" : "normal"));
	}
	else
	{
		ui->cpuUsageValueLabel->setText(QStringLiteral("—"));
	}
	if (!std::isfinite(peakGain))
	{
		setAnalysisStatus(
			analysisStateLabel,
			tr("Analysis is unavailable for the selected device"),
			"warning",
			false);
	}
	else
	{
		setAnalysisStatus(
			analysisStateLabel,
			peakGain > 0
				? tr("Clipping risk detected · lower preamp gain")
				: tr("Response analysis is up to date"),
			peakGain > 0 ? "danger" : "normal",
			false);
	}

	analysisThread->endGetResult();
	refreshAutoPreampActionState();
}

void MainWindow::on_mainToolBar_visibilityChanged(bool visible)
{
	ui->actionToolbar->setChecked(visible);
}

void MainWindow::on_analysisDockWidget_visibilityChanged(bool visible)
{
	ui->actionAnalysisPanel->setChecked(visible);

	if (visible)
		startAnalysis();
}

void MainWindow::on_actionToolbar_triggered(bool checked)
{
	ui->mainToolBar->setVisible(checked);
	if (workspaceToolBar != NULL)
		workspaceToolBar->setVisible(checked);
}

void MainWindow::on_actionAnalysisPanel_triggered(bool checked)
{
	ui->analysisDockWidget->setVisible(checked);
}

void MainWindow::dockAnalysisPanel()
{
	// addDockWidget() also recovers a panel whose previous saved dock location
	// is no longer valid (for example after a display/layout change).
	addDockWidget(Qt::BottomDockWidgetArea, ui->analysisDockWidget);
	ui->analysisDockWidget->setFloating(false);
	ui->analysisDockWidget->show();
	ui->analysisDockWidget->raise();
	dockAnalysisPanelAction->setEnabled(false);
}

void MainWindow::lowerPreampToPreventClipping()
{
	if (!analysisResultCanAdjustPreamp())
	{
		showWorkspaceStatus(
			tr("Run a fresh Current file analysis before adjusting Preamp"),
			"warning");
		return;
	}

	FilterTable* filterTable = currentFilterTable();
	const double reductionDb = conservativePreampReduction(latestAnalysisPeakGain);
	FilterTable::PreampAdjustmentPlan plan;
	if (filterTable == NULL
		|| !filterTable->planPreampReduction(reductionDb, &plan))
	{
		showWorkspaceStatus(
			tr("Auto preamp cannot safely edit this configuration; add or adjust a root Preamp manually"),
			"warning");
		return;
	}

	const QString channelName = ui->analysisChannelComboBox->currentText().isEmpty()
		? tr("selected channel")
		: ui->analysisChannelComboBox->currentText();
	QMessageBox confirmation(this);
	confirmation.setWindowTitle(tr("Apply estimated Preamp safety cut"));
	confirmation.setIcon(QMessageBox::Warning);
	if (plan.insertsNewPreamp)
	{
		confirmation.setText(tr(
			"The latest analysis estimates a +%1 dB peak for %2.\n\n"
			"No root Preamp with enough editable range was found. Add a new %3 dB Preamp at the beginning of this file?")
			.arg(latestAnalysisPeakGain, 0, 'f', 2)
			.arg(channelName)
			.arg(plan.targetDbGain, 0, 'f', 2));
	}
	else
	{
		confirmation.setText(tr(
			"The latest analysis estimates a +%1 dB peak for %2.\n\n"
			"Change the first editable root Preamp from %3 dB to %4 dB?")
			.arg(latestAnalysisPeakGain, 0, 'f', 2)
			.arg(channelName)
			.arg(plan.oldDbGain, 0, 'f', 2)
			.arg(plan.targetDbGain, 0, 'f', 2));
	}
	confirmation.setInformativeText(tr(
		"This one-time cut uses the sampled linear response and current Windows-volume snapshot for the selected channel. "
		"It is not a limiter and cannot guarantee later volume or source changes, nonlinear processing, or intersample peaks. "
		"Review the edit and save it manually."));
	confirmation.setStandardButtons(QMessageBox::Apply | QMessageBox::Cancel);
	confirmation.setDefaultButton(QMessageBox::Cancel);
	confirmation.setEscapeButton(QMessageBox::Cancel);
	if (confirmation.exec() != QMessageBox::Apply)
		return;

	// The modal dialog runs an event loop. Revalidate the request and compare
	// the plan before touching the file in case it changed while the question
	// was open.
	FilterTable::PreampAdjustmentPlan currentPlan;
	if (!analysisResultCanAdjustPreamp()
		|| !filterTable->planPreampReduction(reductionDb, &currentPlan)
		|| currentPlan.insertsNewPreamp != plan.insertsNewPreamp
		|| currentPlan.itemIndex != plan.itemIndex
		|| currentPlan.originalLine != plan.originalLine
		|| qAbs(currentPlan.targetDbGain - plan.targetDbGain) >= 0.005)
	{
		showWorkspaceStatus(
			tr("The profile or analysis changed; no Preamp adjustment was made"),
			"warning");
		return;
	}
	// Auto preamp is intentionally an editor transaction even when Instant mode
	// is enabled. Leaving the result dirty prevents an automatic save from
	// overwriting a file that another process changes in the final race window.
	QScopedValueRollback<bool> adjustmentGuard(applyingAutoPreampAdjustment, true);
	if (!filterTable->applyPreampReduction(currentPlan))
	{
		showWorkspaceStatus(
			tr("The profile or analysis changed; no Preamp adjustment was made"),
			"warning");
		return;
	}

	// Auto preamp deliberately leaves the editor dirty in every mode. This makes
	// the final disk write an explicit user action and prevents Instant mode from
	// overwriting a configuration changed during the last race window.
	const double appliedReductionDb = currentPlan.oldDbGain - currentPlan.targetDbGain;
	showWorkspaceStatus(tr("Reduced Preamp by %1 dB for %2; save the file to apply it")
		.arg(appliedReductionDb, 0, 'f', 2)
		.arg(channelName));
}

void MainWindow::languageSelected(bool selected)
{
	QAction* action = qobject_cast<QAction*>(sender());

	if (!selected)
	{
		action->setChecked(true);
		return;
	}

	QString localeName = action->data().toString();

	if (QMessageBox::question(this, tr("Restart required"), tr("Configuration Editor will be restarted to apply the changed settings. Proceed?")) == QMessageBox::Yes)
	{
		QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
		if (localeName.isEmpty())
		{
			settings.remove("language");
		}
		else
		{
			settings.setValue("language", localeName);
		}

		restart = true;
		close();
	}
	else
	{
		action->setChecked(false);
	}
}

void MainWindow::on_actionResetAllGlobalPreferences_triggered()
{
	if (QMessageBox::question(this, tr("Restart required"), tr("Configuration Editor will be restarted to apply the changed settings. Proceed?")) == QMessageBox::Yes)
	{
		if (!restoreTemporaryProcessingState())
			return;
		QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
		for (const QString& key : settings.childGroups())
		{
			if (key != "file-specific" && key != "temporaryProcessing")
				settings.remove(key);
		}
		for (const QString& key : settings.childKeys())
			settings.remove(key);

		restart = true;
		noSavePreferences = true;
		close();
	}
}

void MainWindow::on_actionResetAllFileSpecificPreferences_triggered()
{
	if (QMessageBox::question(this, tr("Restart required"), tr("Configuration Editor will be restarted to apply the changed settings. Proceed?")) == QMessageBox::Yes)
	{
		QSettings settings(QString::fromWCharArray(EDITOR_PER_FILE_REGPATH), QSettings::NativeFormat);
		for (const QString& key : settings.childGroups())
			settings.remove(key);
		for (const QString& key : settings.childKeys())
			settings.remove(key);

		restart = true;
		noSaveFilePreferences = true;
		close();
	}
}

FilterTable* MainWindow::addTab(QString title, QString tooltip, QString configPath, QList<QString> lines)
{
	QScrollArea* scrollArea = new QScrollArea(ui->tabWidget);
	scrollArea->setWidgetResizable(true);
	FilterTable* filterTable = new FilterTable(this);
	scrollArea->setWidget(filterTable);
	filterTable->setAcceptDrops(true);
	filterTable->setFocusPolicy(Qt::WheelFocus);

	shared_ptr<AbstractAPOInfo> selectedDevice;
	int channelMask;
	getDeviceAndChannelMask(&selectedDevice, &channelMask);
	filterTable->updateDeviceAndChannelMask(selectedDevice, channelMask);
	filterTable->initialize(scrollArea, outputDevices, inputDevices);
	filterTable->setLines(configPath, lines);

	int tabIndex = ui->tabWidget->addTab(scrollArea, title);
	ui->tabWidget->setTabToolTip(tabIndex, tooltip);

	return filterTable;
}

bool MainWindow::loadSnapshotScenario(const QString& scenario)
{
#ifdef EQAPO_ENABLE_UI_SNAPSHOTS
	const auto invalid = [](int line) {
		std::fprintf(stderr, "UI snapshot scenario failed at MainWindow.cpp:%d\n", line);
		return false;
	};
	const bool volumeScenario = scenario == QStringLiteral("volume-follow");
	const bool denseScenario = scenario == QStringLiteral("dense") || volumeScenario;
	const bool restoredToolsScenario = scenario == QStringLiteral("restored-tools");
	if ((!denseScenario && !restoredToolsScenario) || !isEmpty())
		return invalid(__LINE__);

	if (restoredToolsScenario)
	{
		// Exercise the production state-version gate while the window is
		// visible. A version-0 state can contain the merged toolbar layout used
		// by earlier builds; version 1 must reject it without changing the
		// current two-row layout.
		const QByteArray currentWindowState = saveWindowLayoutState();
		removeToolBarBreak(workspaceToolBar);
		const QByteArray legacyMergedToolbarState = saveState(0);
		if (toolBarBreak(workspaceToolBar))
			return invalid(__LINE__);
		if (!restoreWindowLayoutState(currentWindowState))
			return invalid(__LINE__);
		if (!toolBarBreak(workspaceToolBar))
			return invalid(__LINE__);
		if (restoreWindowLayoutState(legacyMergedToolbarState))
			return invalid(__LINE__);
		if (!toolBarBreak(workspaceToolBar))
			return invalid(__LINE__);
	}

	// Keep this representative configuration entirely in memory. In
	// particular, do not call FilterTable::setLines(), because that method
	// intentionally restores the real user's per-file QSettings.
	const QList<QString> lines = denseScenario ? QList<QString>{
		QStringLiteral("LoudnessCorrectionOriginal: Schema 1 Model MixomoShelfV1 State 1 ReferenceLevel 0 ReferenceOffset 0 Attenuation 1.0"),
		QStringLiteral("LoudnessCorrection: Schema 1 Model FormulaLoudnessV1 Binding Single State 1 ReferenceLevel 80 ReferenceOffset 40 Attenuation 1.0 Volume -38.0 VolumeFollow Linear"),
		QStringLiteral("Filter: ON PK Fc 1000 Hz Gain -3 dB Q 1"),
		QStringLiteral("UnsupportedSnapshotCommand: this-deliberately-long-unknown-command-keeps-the-raw-text-middle-elision-and-tooltip-path-covered"),
		QStringLiteral("VSTPlugin: Library snapshot-memory\\plugins\\this-deliberately-long-vst-plugin-library-name-keeps-real-world-path-layout-covered-without-loading-a-file.dll"),
		QStringLiteral("Device: SNAPSHOT-MISSING-PLAYBACK-DEVICE-{0A47-4739-4500-4250-93BA-6A6095B56AEC}"),
		QStringLiteral("# Preamp: -10.30 dB"),
		QStringLiteral("Convolution: snapshot-memory\\impulses\\missing-reference-response.wav"),
		QStringLiteral("Include: snapshot-memory\\profiles\\this-intentionally-long-missing-configuration-name-keeps-real-world-path-layout-covered.txt"),
		QStringLiteral("Preamp: -12.20 dB"),
		QStringLiteral("# Convolution: snapshot-memory\\impulses\\disabled-reference-response.wav"),
		QStringLiteral("# Include: snapshot-memory\\profiles\\disabled-reference-profile.txt"),
		QStringLiteral("Device: all"),
		QStringLiteral("Preamp: -10.40 dB"),
		QStringLiteral("Convolution: snapshot-memory\\impulses\\missing-secondary-reference-response.wav"),
		QStringLiteral("Include: snapshot-memory\\profiles\\missing-secondary-reference-profile.txt"),
		QStringLiteral("Device: SNAPSHOT-MISSING-CAPTURE-DEVICE-{B3D1-48C7-A2F0-118D-98B737227C61}"),
		QStringLiteral("# Preamp: -13.70 dB"),
		QStringLiteral("Include: snapshot-memory\\profiles\\another-deliberately-long-missing-profile-name-for-horizontal-overflow-regression.txt")
	} : QList<QString>{
		QStringLiteral("Preamp: -100 dB"),
		QStringLiteral("Delay: 250 ms"),
		QStringLiteral("Copy: L=L R=R"),
		QStringLiteral("Preamp: 100 dB"),
		QStringLiteral("Pan: Position 0 Width 100"),
		QStringLiteral("Crossfeed: Algorithm Natural Preset \"Average Male\" Amount 35 % Circumference 57 cm HeadWidth 15 cm HeadLength 19 cm Angle 60 deg Cutoff 900 Hz Direct 100 %"),
		QStringLiteral("Chorus: Rate 0.4 Hz Depth 8 ms Mix 25 % Feedback 0 %"),
		QStringLiteral("Reverb: RoomSize 50 % Damping 50 % Wet 20 % Dry 100 % Width 100 %"),
		QStringLiteral("ToneGenerator: State 0 Type Sine Frequency 1000 Hz Level -20 dB Channels all Mode Replace"),
		QStringLiteral("VUMeter: MeterId snapshot-meter Channels all RMS \"AES17\" LUFS \"ITU-R BS.1770-5\""),
		QStringLiteral("ParametricEQ: ON PK Fc 1000 Hz Gain -3 dB Q 1 ON LS Fc 120 Hz Gain 2 dB Q 0.7"),
		QStringLiteral("HeadphoneCalibration:"),
		QStringLiteral("OutProcVSTPlugin: Library snapshot-memory\\plugins\\restored-out-of-process-vst3-effect.vst3 HostId snapshot-outproc")
	};

	QScrollArea* scrollArea = new QScrollArea(ui->tabWidget);
	scrollArea->setWidgetResizable(true);
	FilterTable* filterTable = new FilterTable(this);
	scrollArea->setWidget(filterTable);
	filterTable->setAcceptDrops(true);
	filterTable->setFocusPolicy(Qt::WheelFocus);

	shared_ptr<AbstractAPOInfo> selectedDevice;
	int channelMask;
	getDeviceAndChannelMask(&selectedDevice, &channelMask);
	filterTable->updateDeviceAndChannelMask(selectedDevice, channelMask);
	filterTable->initialize(scrollArea, outputDevices, inputDevices);
	filterTable->setConfigPath(denseScenario
		? QStringLiteral(":/snapshot/dense-real-world.txt")
		: QStringLiteral(":/snapshot/restored-tools.txt"));
	for (const QString& line : lines)
		filterTable->addLine(line);
	filterTable->updateGuis();

	const QString snapshotTitle = denseScenario
		? QStringLiteral("dense-real-world.txt")
		: QStringLiteral("restored-tools.txt");
	const int tabIndex = ui->tabWidget->addTab(scrollArea, snapshotTitle);
	ui->tabWidget->setTabToolTip(
		tabIndex, QStringLiteral("In-memory UI regression scenario"));
	ui->tabWidget->setCurrentIndex(tabIndex);
	connect(filterTable, SIGNAL(linesChanged()), this, SLOT(linesChanged()));

	// Fail the capture instead of silently accepting another empty-workspace
	// image when a parser or GUI factory stops recognizing the fixture.
	const auto objectCount = [this](const QString& objectName)
	{
		return findChildren<QWidget*>(objectName).size();
	};
	bool complete = filterTable->getLines().size() == lines.size()
		&& objectCount(QStringLiteral("FilterTableRow")) == lines.size();
	if (denseScenario)
	{
		complete = complete
			&& objectCount(QStringLiteral("DeviceFilterGUI")) == 3
			&& objectCount(QStringLiteral("PreampFilterGUI")) == 4
			&& objectCount(QStringLiteral("ConvolutionFilterGUI")) == 3
			&& objectCount(QStringLiteral("IncludeFilterGUI")) == 4
			&& objectCount(QStringLiteral("BiQuadFilterGUI")) == 1
			&& objectCount(QStringLiteral("LoudnessCorrectionFilterGUI")) == 1
			&& objectCount(QStringLiteral("OriginalLoudnessCorrectionFilterGUI")) == 1
			&& objectCount(QStringLiteral("VSTPluginFilterGUI")) == 1
			&& objectCount(QStringLiteral("elidingCommandLabel")) == 1;
	}
	else
	{
		complete = complete
			&& objectCount(QStringLiteral("PreampFilterGUI")) == 2
			&& objectCount(QStringLiteral("DelayFilterGUI")) == 1
			&& objectCount(QStringLiteral("CopyFilterGUI")) == 1;
		const QStringList restoredGuiObjectNames = {
			QStringLiteral("PanFilterGUI"),
			QStringLiteral("CrossfeedFilterGUI"),
			QStringLiteral("ChorusFilterGUI"),
			QStringLiteral("ReverbFilterGUI"),
			QStringLiteral("ToneGeneratorFilterGUI"),
			QStringLiteral("VUMeterFilterGUI"),
			QStringLiteral("ParametricEQFilterGUI"),
			QStringLiteral("HeadphoneCalibrationFilterGUI"),
			QStringLiteral("VSTPluginFilterGUI")
		};
		for (const QString& objectName : restoredGuiObjectNames)
			complete = complete && objectCount(objectName) == 1;
	}
	if (!complete)
		return invalid(__LINE__);

	if (denseScenario)
	{
		// Exercise the same inner-GUI target used by both calibration buttons.
		// FilterTable stores a CommentFilterGUI decorator for each row, so this
		// fails unless descendant targets are mapped back to their owning item.
		const QString overrideMarker = QStringLiteral("SnapshotCalibrationOverride");
		for (const QString& objectName : {
			QStringLiteral("LoudnessCorrectionFilterGUI"),
			QStringLiteral("OriginalLoudnessCorrectionFilterGUI") })
		{
			IFilterGUI* innerGui = findChild<IFilterGUI*>(objectName);
			QList<QString> overriddenLines;
			if (innerGui == NULL || !filterTable->makeLinesWithGuiOverride(
				innerGui, overrideMarker, objectName, &overriddenLines) ||
				overriddenLines.size() != lines.size() ||
				overriddenLines.count(overrideMarker + ": " + objectName) != 1)
			{
				return invalid(__LINE__);
			}
		}
	}
	else
	{
		// Exercise an actual Qt row-widget teardown/recreation. Stateful VST
		// process tracking must move through FilterTable::Item rather than the
		// persisted row preferences; otherwise any structural edit strands the
		// live host and its random state sidecar in the deleted widget.
		IFilterGUI* oldVstGui = findChild<IFilterGUI*>(
			QStringLiteral("VSTPluginFilterGUI"));
		QVariantMap expectedRuntimeState;
		expectedRuntimeState.insert(
			QStringLiteral("outProcHostId"),
			QStringLiteral("snapshot-outproc"));
		expectedRuntimeState.insert(
			QStringLiteral("outProcConfigPath"),
			QStringLiteral("C:/snapshot/retained-vst-state.opvs"));
		expectedRuntimeState.insert(
			QStringLiteral("outProcPid"), QStringLiteral("424242"));
		expectedRuntimeState.insert(
			QStringLiteral("outProcProcessCreationTime"),
			QStringLiteral("133713371337"));
		expectedRuntimeState.insert(
			QStringLiteral("outProcRunning"), true);
		expectedRuntimeState.insert(
			QStringLiteral("outProcHidden"), false);
		expectedRuntimeState.insert(
			QStringLiteral("outProcFinalStateCommitted"), false);
		if (oldVstGui == NULL)
			return invalid(__LINE__);
		oldVstGui->restoreRuntimeState(expectedRuntimeState);
		filterTable->updateGuis();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		IFilterGUI* rebuiltVstGui = findChild<IFilterGUI*>(
			QStringLiteral("VSTPluginFilterGUI"));
		QVariantMap actualRuntimeState;
		if (rebuiltVstGui == NULL || rebuiltVstGui == oldVstGui)
			return invalid(__LINE__);
		rebuiltVstGui->takeRuntimeState(actualRuntimeState);
		if (actualRuntimeState != expectedRuntimeState)
			return invalid(__LINE__);

		FilterTable cloneProbe(this);
		FilterTable::Item* outProcProbe = cloneProbe.addLine(QStringLiteral(
			"OutProcVSTPlugin: Library \"C:/My HostId Plugin.dll\" 0 \"MeterId\" 0.5 HostId old-session HostId stale-session"));
		cloneProbe.cloneItem(outProcProbe, true);
		FilterTable::Item* meterProbe = cloneProbe.addLine(QStringLiteral(
			"VUMeter: Label \"My MeterId View\" MeterId old-meter Channels all"));
		cloneProbe.cloneItem(meterProbe, true);
		FilterTable::Item* danglingOutProcProbe = cloneProbe.addLine(QStringLiteral(
			"OutProcVSTPlugin: Library dangling.vst3 HostId"));
		cloneProbe.cloneItem(danglingOutProcProbe, true);
		FilterTable::Item* emptyOutProcProbe = cloneProbe.addLine(QStringLiteral(
			"OutProcVSTPlugin: Library empty.vst3 HostId \"\""));
		cloneProbe.cloneItem(emptyOutProcProbe, true);
		FilterTable::Item* danglingMeterProbe = cloneProbe.addLine(QStringLiteral(
			"VUMeter: Channels all MeterId"));
		cloneProbe.cloneItem(danglingMeterProbe, true);
		FilterTable::Item* emptyMeterProbe = cloneProbe.addLine(QStringLiteral(
			"VUMeter: Channels all MeterId \"\""));
		cloneProbe.cloneItem(emptyMeterProbe, true);
		const QList<QString> clonedLines = cloneProbe.getLines();
		if (clonedLines.size() != 12 ||
			!clonedLines[1].contains(QStringLiteral(
				"Library \"C:/My HostId Plugin.dll\"")) ||
			!clonedLines[1].contains(QStringLiteral("0 \"MeterId\" 0.5")) ||
			clonedLines[1].contains(QStringLiteral("old-session")) ||
			clonedLines[1].contains(QStringLiteral("stale-session")) ||
			!clonedLines[3].contains(QStringLiteral(
				"Label \"My MeterId View\"")) ||
			clonedLines[3].contains(QStringLiteral("old-meter")) ||
			!clonedLines[5].startsWith(QStringLiteral(
				"OutProcVSTPlugin: HostId ")) ||
			clonedLines[5].count(QStringLiteral("HostId")) != 1 ||
			!clonedLines[7].startsWith(QStringLiteral(
				"OutProcVSTPlugin: HostId ")) ||
			clonedLines[7].count(QStringLiteral("HostId")) != 1 ||
			clonedLines[7].contains(QStringLiteral("\"\"")) ||
			!clonedLines[9].startsWith(QStringLiteral("VUMeter: MeterId ")) ||
			clonedLines[9].count(QStringLiteral("MeterId")) != 1 ||
			!clonedLines[11].startsWith(QStringLiteral("VUMeter: MeterId ")) ||
			clonedLines[11].count(QStringLiteral("MeterId")) != 1 ||
			clonedLines[11].contains(QStringLiteral("\"\"")))
		{
			return invalid(__LINE__);
		}

		// Invalid legacy HostIds must be canonicalized to distinct UUIDs before
		// object lookup, or punctuation replacement could alias two GUI sessions.
		QScrollArea canonicalProbeArea;
		canonicalProbeArea.setWidgetResizable(true);
		FilterTable* canonicalProbe = new FilterTable(this);
		canonicalProbeArea.setWidget(canonicalProbe);
		canonicalProbe->initialize(
			&canonicalProbeArea, outputDevices, inputDevices);
		canonicalProbe->addLine(QStringLiteral(
			"OutProcVSTPlugin: Library invalid.vst3 HostId foo!"));
		canonicalProbe->addLine(QStringLiteral(
			"OutProcVSTPlugin: Library invalid.vst3 HostId foo?"));
		canonicalProbe->updateGuis();
		QCoreApplication::processEvents();
		const QList<QString> canonicalLines = canonicalProbe->getLines();
		auto serializedHostId = [](const QString& line)
		{
			const QString marker = QStringLiteral(" HostId ");
			const int offset = line.indexOf(marker);
			return offset < 0 ? QString() :
				line.mid(offset + marker.size()).section(' ', 0, 0);
		};
		const QString firstCanonicalId = canonicalLines.size() > 0
			? serializedHostId(canonicalLines[0]) : QString();
		const QString secondCanonicalId = canonicalLines.size() > 1
			? serializedHostId(canonicalLines[1]) : QString();
		if (canonicalLines.size() != 2 || firstCanonicalId.isEmpty() ||
			secondCanonicalId.isEmpty() || firstCanonicalId == secondCanonicalId ||
			firstCanonicalId.contains('!') || secondCanonicalId.contains('?'))
		{
			return invalid(__LINE__);
		}
	}

	if (volumeScenario)
	{
		// A focused capture must actually expose the changed row. The dense
		// matrix intentionally gives most vertical space to the analysis panel.
		ui->analysisDockWidget->hide();
		QTimer::singleShot(100, this, [scrollArea, filterTable]() {
			QWidget* row = filterTable->findChild<QWidget*>(
				QStringLiteral("LoudnessCorrectionFilterGUI"));
			if (row != NULL)
				scrollArea->verticalScrollBar()->setValue(qMax(0, row->mapTo(filterTable, QPoint(0, 0)).y() - GUIHelper::scale(20)));
		});
	}
	{
		QScrollArea precisionScroll;
		auto* precisionTable = new FilterTable(this);
		precisionScroll.setWidget(precisionTable);
		precisionTable->initialize(&precisionScroll, outputDevices, inputDevices);
		precisionTable->addLine(QStringLiteral("Preamp: -3 dB"));
		if (!precisionTable->setDoublePrecision(false)
			|| precisionTable->getLines().first() != QStringLiteral("ProcessingPrecision: 32"))
			return invalid(__LINE__);
		if (!precisionTable->setDoublePrecision(true)
			|| precisionTable->getLines().size() != 2
			|| precisionTable->getLines().first() != QStringLiteral("ProcessingPrecision: 64"))
			return invalid(__LINE__);
		precisionTable->addLine(QStringLiteral("Device: Speakers"));
		if (!precisionTable->setDoublePrecision(false)
			|| precisionTable->getLines().size() != 3
			|| precisionTable->getLines().first() != QStringLiteral("ProcessingPrecision: 32"))
			return invalid(__LINE__);
		precisionTable->addLine(QStringLiteral("Include: scoped.txt"));
		const QList<QString> scopedLines = precisionTable->getLines();
		if (precisionTable->setDoublePrecision(false) || precisionTable->getLines() != scopedLines)
			return invalid(__LINE__);
	}
	refreshWorkspaceActionState();
	return true;
#else
	Q_UNUSED(scenario);
	return false;
#endif
}

bool MainWindow::snapshotLayoutIsValid() const
{
#ifdef EQAPO_ENABLE_UI_SNAPSHOTS
	const auto invalid = [](int line) {
		std::fprintf(stderr, "UI snapshot layout validation failed at MainWindow.cpp:%d\n", line);
		return false;
	};
	const bool denseScenario = UiSnapshot::scenario() == QStringLiteral("dense") ||
		UiSnapshot::scenario() == QStringLiteral("volume-follow");
	const bool restoredToolsScenario = UiSnapshot::scenario() == QStringLiteral("restored-tools");
	if (!denseScenario && !restoredToolsScenario)
		return true;
	if (workspaceToolBar == NULL
		|| toolBarArea(ui->mainToolBar) != Qt::TopToolBarArea
		|| toolBarArea(workspaceToolBar) != Qt::TopToolBarArea
		|| !toolBarBreak(workspaceToolBar))
		return invalid(__LINE__);

	QScrollArea* scrollArea = qobject_cast<QScrollArea*>(
		ui->tabWidget->currentWidget());
	if (scrollArea == NULL || scrollArea->horizontalScrollBar() == NULL
		|| scrollArea->horizontalScrollBar()->maximum() != 0)
		return invalid(__LINE__);

	if (denseScenario)
	{
		// Exercise real widgets without attaching them to a file or audio host.
		// An invalid Single binding must never fall back to the default endpoint.
		using Parameters = LoudnessCorrectionFilter::FilterParameters;
		LoudnessCorrectionFilterGUI probe(true, 80, 0, 1,
			Parameters::BINDING_SINGLE, true, -50,
			Parameters::ENGINE_FULL, Parameters::VOLUME_FOLLOW_LINEAR, L"", false);
		auto target = probe.findChild<QLabel*>(QStringLiteral("volumeFollowStatusLabel"));
		auto curve = probe.findChild<QComboBox*>(QStringLiteral("volumeFollowComboBox"));
		auto manual = probe.findChild<QCheckBox*>(QStringLiteral("manualVolumeCheckBox"));
		auto volume = probe.findChild<QDoubleSpinBox*>(QStringLiteral("volumeSpinBox"));
		if (!target || !curve || !manual || !volume || !target->text().contains("-6.02"))
			return invalid(__LINE__);
		curve->setCurrentIndex(2);
		if (!target->text().contains("-12.04")) return invalid(__LINE__);
		curve->setCurrentIndex(3);
		if (!target->text().contains("-50.00")) return invalid(__LINE__);
		volume->setValue(-20);
		if (!target->text().contains("-20.00")) return invalid(__LINE__);
		manual->setChecked(false);
		QString command, parameters;
		probe.store(command, parameters);
		if (parameters.contains(" Volume ") || !parameters.contains("Binding Single") ||
			target->text() != QCoreApplication::translate("LoudnessCorrectionFilterGUI",
				"APO follow target: unknown (source unavailable)")) return invalid(__LINE__);
		curve->setCurrentIndex(0);
		if (!target->text().contains("0.00")) return invalid(__LINE__);

		const QList<QWidget*> deviceRows = findChildren<QWidget*>(
			QStringLiteral("DeviceFilterGUI"));
		if (deviceRows.size() != 3)
			return invalid(__LINE__);
		for (QWidget* deviceRow : deviceRows)
		{
			QAbstractScrollArea* tree = deviceRow->findChild<QAbstractScrollArea*>(
				QStringLiteral("treeWidget"));
			if (tree == NULL || tree->horizontalScrollBar() == NULL
				|| tree->horizontalScrollBar()->maximum() != 0)
				return invalid(__LINE__);
		}
	}

	// The table deliberately accepts a zero horizontal minimum so wide text
	// can compress instead of forcing an outer scrollbar. A zero scrollbar
	// range alone therefore cannot prove that a child was not clipped. Check
	// the representative GUIs and every visible descendant against the owning
	// filter GUI. Mapping to that stable boundary avoids false positives from
	// native-style implementation widgets that intentionally overlap frames.
	const auto fitsInsideContainer = [](QWidget* container, QWidget* widget)
	{
		const QRect geometryInContainer = container == NULL || widget == NULL
			? QRect()
			: QRect(widget->mapTo(container, QPoint(0, 0)), widget->size());
		const bool fits = container != NULL
			&& widget->width() > 0 && widget->height() > 0
			&& container->rect().contains(geometryInContainer);
		if (!fits)
		{
			std::fprintf(stderr, "Geometry: %s (%d,%d %dx%d) in %s (%dx%d)\n", widget ? qPrintable(widget->objectName()) : "null", geometryInContainer.x(), geometryInContainer.y(), geometryInContainer.width(), geometryInContainer.height(), container ? qPrintable(container->objectName()) : "null", container ? container->width() : 0, container ? container->height() : 0);
			qWarning().noquote()
				<< "Dense snapshot geometry violation:"
				<< (widget == NULL
					? QStringLiteral("<null>")
					: QString::fromLatin1(widget->metaObject()->className()))
				<< (widget == NULL ? QString() : widget->objectName())
				<< "geometry" << geometryInContainer
				<< "container"
				<< (container == NULL
					? QStringLiteral("<null>")
					: QString::fromLatin1(container->metaObject()->className()))
				<< (container == NULL ? QString() : container->objectName())
				<< "containerRect"
				<< (container == NULL ? QRect() : container->rect());
		}
		return fits;
	};
	if (autoPreampButton == NULL
		|| !fitsInsideContainer(autoPreampButton->parentWidget(), autoPreampButton))
		return invalid(__LINE__);
	const QList<QWidget*> analysisControls = {
		ui->startFromLabel, ui->startFromComboBox,
		ui->analysisChannelLabel, ui->analysisChannelComboBox,
		ui->resolutionLabel, ui->resolutionSpinBox, ui->resetAnalysisViewButton
	};
	for (QWidget* control : analysisControls)
		if (!fitsInsideContainer(ui->groupBox, control))
			return invalid(__LINE__);
	const QStringList simpleGuiObjectNames = denseScenario ? QStringList{
		QStringLiteral("DeviceFilterGUI"),
		QStringLiteral("PreampFilterGUI"),
		QStringLiteral("ConvolutionFilterGUI"),
		QStringLiteral("IncludeFilterGUI"),
		QStringLiteral("BiQuadFilterGUI"),
		QStringLiteral("LoudnessCorrectionFilterGUI"),
		QStringLiteral("OriginalLoudnessCorrectionFilterGUI"),
		QStringLiteral("VSTPluginFilterGUI"),
		QStringLiteral("elidingCommandLabel")
	} : QStringList{
		QStringLiteral("PreampFilterGUI"),
		QStringLiteral("DelayFilterGUI"),
		QStringLiteral("CopyFilterGUI"),
		QStringLiteral("PanFilterGUI"),
		QStringLiteral("CrossfeedFilterGUI"),
		QStringLiteral("ChorusFilterGUI"),
		QStringLiteral("ReverbFilterGUI"),
		QStringLiteral("ToneGeneratorFilterGUI"),
		QStringLiteral("VUMeterFilterGUI"),
		QStringLiteral("VSTPluginFilterGUI")
	};
	for (const QString& objectName : simpleGuiObjectNames)
	{
		const QList<QWidget*> guis = findChildren<QWidget*>(objectName);
		if (guis.isEmpty())
			return invalid(__LINE__);
		for (QWidget* gui : guis)
		{
			if (!fitsInsideContainer(gui->parentWidget(), gui))
				return invalid(__LINE__);
			for (QWidget* child : gui->findChildren<QWidget*>())
			{
				if (child->isVisible() && !child->isWindow()
					&& !fitsInsideContainer(gui, child))
				{
					return invalid(__LINE__);
				}
			}
		}
	}
	if (restoredToolsScenario)
	{
		QWidget* copyGui = findChild<QWidget*>(QStringLiteral("CopyFilterGUI"));
		QWidget* copyReset = copyGui == NULL
			? NULL
			: copyGui->findChild<QWidget*>(QStringLiteral("copyResetButton"));
		QWidget* copyTitle = copyGui == NULL
			? NULL
			: copyGui->findChild<QWidget*>(QStringLiteral("copyLabel"));
		QWidget* copyTabs = copyGui == NULL
			? NULL
			: copyGui->findChild<QWidget*>(QStringLiteral("tabWidget"));
		if (copyReset == NULL || copyTitle == NULL || copyTabs == NULL
			|| copyReset->sizePolicy().horizontalPolicy() != QSizePolicy::Fixed)
			return invalid(__LINE__);
		const QRect resetRect(copyReset->mapTo(copyGui, QPoint(0, 0)), copyReset->size());
		const QRect titleRect(copyTitle->mapTo(copyGui, QPoint(0, 0)), copyTitle->size());
		const QRect tabsRect(copyTabs->mapTo(copyGui, QPoint(0, 0)), copyTabs->size());
		if (resetRect.intersects(titleRect) || tabsRect.right() < resetRect.right())
			return invalid(__LINE__);
	}

	if (restoredToolsScenario)
	{
		const QPair<QString, QString> scrollChecks[] = {
			qMakePair(QStringLiteral("ParametricEQFilterGUI"), QStringLiteral("parametricEQScrollArea")),
			qMakePair(QStringLiteral("HeadphoneCalibrationFilterGUI"), QStringLiteral("headphoneCalibrationScrollArea"))
		};
		for (const QPair<QString, QString>& check : scrollChecks)
		{
			QWidget* gui = findChild<QWidget*>(check.first);
			QScrollArea* internalScroll = gui == NULL
				? NULL
				: gui->findChild<QScrollArea*>(check.second);
			QWidget* content = internalScroll == NULL ? NULL : internalScroll->widget();
			if (!fitsInsideContainer(gui == NULL ? NULL : gui->parentWidget(), gui)
				|| internalScroll == NULL || content == NULL
				|| internalScroll->horizontalScrollBar() == NULL
				|| internalScroll->viewport() == NULL
				|| !fitsInsideContainer(gui, internalScroll))
				return invalid(__LINE__);

			const bool contentOverflows = content->width() > internalScroll->viewport()->width();
			if (contentOverflows && internalScroll->horizontalScrollBar()->maximum() <= 0)
				return invalid(__LINE__);
			for (QWidget* child : content->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
				if (child->isVisible() && !fitsInsideContainer(content, child))
					return invalid(__LINE__);
		}
	}

	// The compact reset controls in the two legacy two-row editors must not
	// consume the stretch column or collide with their value editor. Their
	// component titles stay pinned to the top edge of the row.
	struct CompactResetCheck
	{
		QString guiObjectName;
		QString resetObjectName;
		QString editorObjectName;
		QString titleObjectName;
	};
	const CompactResetCheck compactResetChecks[] = {
		{QStringLiteral("PreampFilterGUI"), QStringLiteral("preampResetButton"),
			QStringLiteral("doubleSpinBox"), QStringLiteral("label")},
		{QStringLiteral("DelayFilterGUI"), QStringLiteral("delayResetButton"),
			QStringLiteral("delaySpinBox"), QStringLiteral("delayLabel")}
	};
	for (const CompactResetCheck& check : compactResetChecks)
	{
		const QList<QWidget*> guis = findChildren<QWidget*>(check.guiObjectName);
		if (denseScenario && check.guiObjectName == QStringLiteral("DelayFilterGUI"))
			continue;
		if (guis.isEmpty())
			return invalid(__LINE__);
		for (QWidget* gui : guis)
		{
			QWidget* reset = gui->findChild<QWidget*>(check.resetObjectName);
			QWidget* editor = gui->findChild<QWidget*>(check.editorObjectName);
			QLabel* title = gui->findChild<QLabel*>(check.titleObjectName);
			if (reset == NULL || editor == NULL || title == NULL
				|| reset->sizePolicy().horizontalPolicy() != QSizePolicy::Fixed
				|| !(title->alignment() & Qt::AlignVCenter))
			{
				return invalid(__LINE__);
			}
			const QRect resetRect(reset->mapTo(gui, QPoint(0, 0)), reset->size());
			const QRect editorRect(editor->mapTo(gui, QPoint(0, 0)), editor->size());
			if (resetRect.intersects(editorRect))
				return invalid(__LINE__);
		}
	}
	return true;
#else
	return true;
#endif
}

void MainWindow::getDeviceAndChannelMask(shared_ptr<AbstractAPOInfo>* selectedDevice, int* channelMask) const
{
	*selectedDevice = deviceComboBox->currentData().value<shared_ptr<AbstractAPOInfo>>();
	if (*selectedDevice == NULL)
		*selectedDevice = defaultOutputDevice;

	*channelMask = channelConfigurationComboBox->currentData().toInt();
	if (*channelMask == 0 && selectedDevice->get() != NULL)
	{
		*channelMask = (*selectedDevice)->getChannelMask();

		if (*channelMask == 0)
			*channelMask = ChannelHelper::getDefaultChannelMask((*selectedDevice)->getChannelCount());
	}
}

bool MainWindow::askForClose(int tabIndex)
{
	bool discarded = false;
	if (ui->tabWidget->tabText(tabIndex).endsWith('*'))
	{
		ui->tabWidget->setCurrentIndex(tabIndex);
		QString configPath = ui->tabWidget->tabToolTip(tabIndex);
		QMessageBox messageBox;
		messageBox.setWindowTitle(tr("Unsaved changes"));
		messageBox.setText(tr("The configuration file %1 has unsaved changes.").arg(configPath));
		messageBox.setInformativeText(tr("Do you want to save the changes before closing the file?"));
		messageBox.setIcon(QMessageBox::Question);
		messageBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
		messageBox.setDefaultButton(QMessageBox::Save);
		messageBox.setEscapeButton(QMessageBox::Cancel);
		messageBox.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
		int result = messageBox.exec();

		switch (result)
		{
		case QMessageBox::Save:
			ui->actionSave->trigger();
			if (ui->tabWidget->tabText(tabIndex).endsWith('*'))
				// saving was canceled
				return false;
			break;
		case QMessageBox::Discard:
			discarded = true;
			break;
		case QMessageBox::Cancel:
			return false;
		}
	}

	if (!discarded && !noSaveFilePreferences)
	{
		QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(tabIndex));
		if (scrollArea != NULL)
		{
			FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
			filterTable->savePreferences();
		}
	}

	return true;
}

void MainWindow::startAnalysis()
{
	invalidateAnalysisResult();
	requestedAnalysisGeneration = 0;
	requestedAnalysisTable.clear();
	requestedAnalysisLinesHash.clear();
	requestedAnalysisConfigPath.clear();
	requestedAnalysisDeviceId.clear();
	requestedAnalysisChannelMask = 0;
	requestedAnalysisChannelIndex = -1;
	requestedAnalysisStartFrom = -1;
	if (UiSnapshot::requested())
		return;
	if (!ui->analysisDockWidget->isVisible())
		return;
	setAnalysisStatus(
		analysisStateLabel,
		tr("Analyzing the current signal path…"),
		"normal",
		true);

	shared_ptr<AbstractAPOInfo> selectedDevice;

	int channelMask;
	getDeviceAndChannelMask(&selectedDevice, &channelMask);

	if (selectedDevice != NULL)
	{
		QString configPath;

		if (ui->startFromComboBox->currentIndex() == 1)
		{
			QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->currentWidget());
			if (scrollArea != NULL)
			{
				FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());

				if (filterTable->getConfigPath().length() > 0)
					configPath = filterTable->getConfigPath();
			}
		}

		if (configPath.isEmpty())
			configPath = configDir.absoluteFilePath("config.txt");
		configPath = QDir::toNativeSeparators(configPath);
		requestedAnalysisConfigPath = configPath;

		FilterTable* filterTable = currentFilterTable();
		if (filterTable != NULL && !filterTable->getConfigPath().isEmpty()
			&& configurationPathKey(filterTable->getConfigPath())
				== configurationPathKey(configPath))
		{
			requestedAnalysisTable = filterTable;
			requestedAnalysisLinesHash = QCryptographicHash::hash(
				serializeConfigurationLines(filterTable->getLines()),
				QCryptographicHash::Sha256);
		}
		requestedAnalysisDeviceId = analysisDeviceId(selectedDevice);
		requestedAnalysisChannelMask = channelMask;
		requestedAnalysisChannelIndex = ui->analysisChannelComboBox->currentIndex();
		requestedAnalysisStartFrom = ui->startFromComboBox->currentIndex();
		requestedAnalysisGeneration = analysisThread->setParameters(
			selectedDevice,
			channelMask,
			requestedAnalysisChannelIndex,
			configPath,
			ui->resolutionSpinBox->value());
	}
	else
	{
		setAnalysisStatus(
			analysisStateLabel,
			tr("Select an installed playback device to run analysis"),
			"warning",
			false);
	}
}

void MainWindow::loadPreferences()
{
	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	QVariant geometryValue = settings.value("geometry");
	if (geometryValue.isValid())
		restoreGeometry(geometryValue.toByteArray());
	instantModeCheckBox->setChecked(settings.value("instantMode", true).toBool());
	closeToTray = settings.value("closeToTray", false).toBool();
	if (closeToTrayAction != NULL)
	{
		QSignalBlocker blocker(closeToTrayAction);
		closeToTrayAction->setChecked(closeToTray);
	}
	takeoverVolumeKeys = settings.value("takeoverVolumeKeys", false).toBool();
	if (takeoverVolumeKeysAction != NULL)
	{
		QSignalBlocker blocker(takeoverVolumeKeysAction);
		takeoverVolumeKeysAction->setChecked(takeoverVolumeKeys);
	}
	VolumeTakeoverManager::instance()->setTakeoverEnabled(takeoverVolumeKeys);
	QString selectedDevice = settings.value("selectedDevice").toString();
	if (!selectedDevice.isEmpty())
	{
		for (int i = 0; i < deviceComboBox->count(); i++)
		{
			shared_ptr<AbstractAPOInfo> apoInfo = deviceComboBox->itemData(i).value<shared_ptr<AbstractAPOInfo>>();
			if (matchesStoredDevice(apoInfo, selectedDevice))
			{
				deviceComboBox->setCurrentIndex(i);
				break;
			}
		}
	}
	deviceSelected(deviceComboBox->currentIndex());

	int selectedChannelMask = settings.value("selectedChannelMask").toInt();
	if (selectedChannelMask != 0)
	{
		int index = channelConfigurationComboBox->findData(selectedChannelMask);
		if (index != -1)
			channelConfigurationComboBox->setCurrentIndex(index);
	}
	channelConfigurationSelected(channelConfigurationComboBox->currentIndex());

	ui->startFromComboBox->setCurrentIndex(settings.value("analysis/startFrom").toInt());
	ui->analysisChannelComboBox->setCurrentText(settings.value("analysis/channel").toString());
	ui->resolutionSpinBox->setValue(settings.value("analysis/resolution", 65536).toInt());
	double zoomX = GUIHelper::scaleZoom(settings.value("analysis/zoomX", 1.0).toDouble());
	double zoomY = GUIHelper::scaleZoom(settings.value("analysis/zoomY", 1.0).toDouble());
	analysisPlotScene->setZoom(zoomX, zoomY);
	bool ok;
	int scrollX = GUIHelper::scale(settings.value("analysis/scrollX").toDouble(&ok));
	if (!ok)
		scrollX = round(analysisPlotScene->hzToX(20));
	int scrollY = GUIHelper::scale(settings.value("analysis/scrollY").toDouble(&ok));
	if (!ok)
		scrollY = round(analysisPlotScene->dbToY(22));

	ui->graphicsView->setScrollOffsets(scrollX, scrollY);

	QVariant openFilesValue = settings.value("openFiles");
	int tabIndex = settings.value("tabIndex").toInt();
	if (openFilesValue.isValid())
	{
		QStringList fileList = openFilesValue.toStringList();
		for (int i = 0; i < fileList.size(); i++)
		{
			load(fileList[i]);
			if (i == tabIndex)
				tabIndex = ui->tabWidget->currentIndex();
		}
	}
	ui->tabWidget->setCurrentIndex(tabIndex);
	recentFiles = settings.value("recentFiles").toStringList();
	updateRecentFiles();

	QVariant languageValue = settings.value("language");
	QString localeName;
	if (languageValue.isValid())
	{
		localeName = languageValue.toString();
		if (localeName == "zh")
			localeName = supportedLocaleName(QLocale::system()).startsWith("zh_")
				? supportedLocaleName(QLocale::system())
				: "zh_CN";
	}

	for (QAction* action : ui->menuLanguage->actions())
		action->setChecked(action->data().toString() == localeName);

	// load window state after initializing channels as it may trigger on_analysisDockWidget_visibilityChanged when analysis panel is detached
	QVariant stateValue = settings.value("windowState");
	if (stateValue.isValid())
		restoreWindowLayoutState(stateValue.toByteArray());
}

void MainWindow::savePreferences()
{
	if (noSavePreferences)
		return;

	QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
	settings.setValue("geometry", saveGeometry());
	settings.setValue("windowState", saveWindowLayoutState());
	settings.setValue("instantMode", instantModeCheckBox->isChecked());
	settings.setValue("closeToTray", closeToTray);
	settings.setValue("takeoverVolumeKeys", takeoverVolumeKeys);
	shared_ptr<AbstractAPOInfo> selectedDevice = deviceComboBox->currentData().value<shared_ptr<AbstractAPOInfo>>();
	settings.setValue("selectedDevice", selectedDevice != NULL ? QString::fromStdWString(selectedDevice->getDeviceGuid().empty() ? selectedDevice->getDeviceString() : selectedDevice->getDeviceGuid()) : "");
	int channelMask = channelConfigurationComboBox->currentData().toInt();
	settings.setValue("selectedChannelMask", channelMask);

	settings.setValue("analysis/startFrom", ui->startFromComboBox->currentIndex());
	settings.setValue("analysis/channel", ui->analysisChannelComboBox->currentText());
	settings.setValue("analysis/resolution", ui->resolutionSpinBox->value());
	settings.setValue("analysis/zoomX", GUIHelper::invScaleZoom(analysisPlotScene->getZoomX()));
	settings.setValue("analysis/zoomY", GUIHelper::invScaleZoom(analysisPlotScene->getZoomY()));
	QScrollBar* hScrollBar = ui->graphicsView->horizontalScrollBar();
	double value = GUIHelper::invScale(hScrollBar->value());
	settings.setValue("analysis/scrollX", value);
	QScrollBar* vScrollBar = ui->graphicsView->verticalScrollBar();
	value = GUIHelper::invScale(vScrollBar->value());
	settings.setValue("analysis/scrollY", value);

	QStringList fileList;
	for (int i = 0; i < ui->tabWidget->count(); i++)
	{
		QScrollArea* scrollArea = qobject_cast<QScrollArea*>(ui->tabWidget->widget(i));
		if (scrollArea == NULL)
			continue;
		FilterTable* filterTable = qobject_cast<FilterTable*>(scrollArea->widget());
		if (filterTable->getConfigPath().length() > 0)
		{
			fileList.append(filterTable->getConfigPath());
		}
	}
	settings.setValue("openFiles", fileList);
	settings.setValue("tabIndex", ui->tabWidget->currentIndex());
	settings.setValue("recentFiles", recentFiles);

	settings.sync();
}

void MainWindow::updateRecentFiles()
{
	QList<QAction*> actions = ui->menuFile->actions();
	int separatorsFound = 0;
	for (int i = actions.size() - 1; i >= 0; i--)
	{
		QAction* action = actions[i];
		if (action->isSeparator())
		{
			separatorsFound++;

			if (separatorsFound == 1)
			{
				QList<QAction*> newActions;
				for (const QString& recentFile : recentFiles)
				{
					QAction* newAction = new QAction(recentFile, ui->menuFile);
					connect(newAction, SIGNAL(triggered(bool)), this, SLOT(recentFileSelected()));
					newActions.append(newAction);
				}
				ui->menuFile->insertActions(action, newActions);
			}
			else
			{
				break;
			}
		}
		else if (separatorsFound >= 1)
		{
			ui->menuFile->removeAction(action);
		}
	}
}

template<class T> QList<T> MainWindow::toQList(const std::vector<T>& vector)
{
	QList<T> list;
	list.reserve((int)vector.size());
	for (T t : vector)
		list.append(t);

	return list;
}
