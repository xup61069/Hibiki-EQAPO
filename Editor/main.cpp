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

#include <QTranslator>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QCommandLineParser>
#include <QSettings>
#include <QStyleHints>
#include <windows.h>
#include "CustomStyle.h"
#include "MainWindow.h"
#include "ModernTheme.h"
#include "helpers/RegistryHelper.h"
#include "helpers/UiSnapshot.h"
#include "Editor/helpers/GUIHelper.h"

using namespace std;

static QString getExecutableDir()
{
	wchar_t path[MAX_PATH] = {};
	DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
		return QDir::currentPath();

	return QFileInfo(QString::fromWCharArray(path, length)).absolutePath();
}

int main(int argc, char* argv[])
{
	int result = -1;
#ifdef _DEBUG
	// _CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
	// _CrtSetBreakAlloc(3318);
#endif

	const QString executableDir = getExecutableDir();
	const QString qtPluginDir = QDir(executableDir).filePath("qt");
	QCoreApplication::addLibraryPath(qtPluginDir);
	qputenv("QT_PLUGIN_PATH", QDir::toNativeSeparators(qtPluginDir).toLocal8Bit());

	bool restart;
	const QByteArray platformScale = qgetenv("QT_SCALE_FACTOR");
	do
	{
		int interfaceScale = 100;
		if (!UiSnapshot::requested())
		{
			QSettings settings(QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
			interfaceScale = qBound(75, settings.value("interfaceScale", 100).toInt(), 200);
			bool valid = false;
			const double externalScale = platformScale.toDouble(&valid);
			qputenv("QT_SCALE_FACTOR", QByteArray::number(
				(valid && externalScale > 0 ? externalScale : 1.0) * interfaceScale / 100.0));
		}
		QApplication application(argc, argv);
		application.setProperty("studioInterfaceScale", interfaceScale);
		application.setWindowIcon(QIcon(QStringLiteral(":/icons/hibiki-editor.ico")));
		application.setStyle("fusion");
		application.setStyle(new CustomStyle(application.style()));
		ModernTheme::install(application);

		const bool snapshotMode = UiSnapshot::requested();
		QVariant languageValue;
		if (!snapshotMode)
		{
			QSettings settings(
				QString::fromWCharArray(EDITOR_REGPATH), QSettings::NativeFormat);
			languageValue = settings.value("language");
		}

		if (snapshotMode)
			QLocale::setDefault(QLocale(UiSnapshot::localeName()));
		else if (languageValue.isValid())
		{
			QString localeName = languageValue.toString();
			if (localeName == "zh")
			{
				QString systemName = QLocale::system().name();
				localeName = systemName.endsWith("_TW") ||
					systemName.endsWith("_HK") || systemName.endsWith("_MO")
					? "zh_TW" : "zh_CN";
			}
			QLocale::setDefault(QLocale(localeName));
		}
		else
			QLocale::setDefault(QLocale::system());

		QTranslator qtTranslator;
		if (qtTranslator.load(QLocale(), ":/translations/qtbase", "_"))
			application.installTranslator(&qtTranslator);

		QTranslator editorTranslator;
		if (editorTranslator.load(QLocale(), ":/translations/Editor", "_"))
			application.installTranslator(&editorTranslator);

		QString configPath = QStringLiteral(":/snapshot");
		if (!snapshotMode)
		{
			configPath = QDir::currentPath();
			if (RegistryHelper::keyExists(APP_REGPATH)
				&& RegistryHelper::valueExists(APP_REGPATH, L"ConfigPath"))
			{
				configPath = QString::fromStdWString(
					RegistryHelper::readValue(APP_REGPATH, L"ConfigPath"));
			}

			if (!RegistryHelper::keyExists(USER_REGPATH))
				RegistryHelper::createKey(USER_REGPATH);
			if (!RegistryHelper::keyExists(EDITOR_REGPATH))
				RegistryHelper::createKey(EDITOR_REGPATH);
			if (!RegistryHelper::keyExists(EDITOR_PER_FILE_REGPATH))
				RegistryHelper::createKey(EDITOR_PER_FILE_REGPATH);
		}
		QDir configDir(configPath);

		MainWindow w(configDir);
		UiSnapshot::prepareForCapture(w);
		w.show();

		QCommandLineParser parser;
		parser.process(application);
		QStringList args = parser.positionalArguments();
		if (args.isEmpty() && w.isEmpty() && !snapshotMode)
			args = QStringList("config.txt");

		for (const QString& arg : args)
			w.load(configDir.absoluteFilePath(arg));

		if (snapshotMode)
		{
			const QString scenario = UiSnapshot::scenario();
			if (!scenario.isEmpty() && !w.loadSnapshotScenario(scenario))
				return 87;
			UiSnapshot::schedule(w, application, [&w]
			{
				return w.snapshotLayoutIsValid();
			});
		}
		else
			w.doChecks();

		result = application.exec();

		restart = w.shouldRestart();
	}
	while (restart);

	return result;
}
