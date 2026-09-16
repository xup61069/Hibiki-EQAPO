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
#include <ObjBase.h>
#include <QtWidgets/QApplication>
#include <VoicemeeterAPOInfo.h>
#include <winsock2.h>
#include "ReceiveThread.h"
#include "DeviceSelector.h"
#include "Editor/ModernTheme.h"
#include "helpers/UiSnapshot.h"

int main(int argc, char* argv[])
{
	int result = 0;

	QCoreApplication::addLibraryPath("qt");

	QApplication app(argc, argv);
	app.setStyle("fusion");
	ModernTheme::install(app);

	QLocale::setDefault(UiSnapshot::requested()
		? QLocale(QStringLiteral("en")) : QLocale::system());

	QTranslator qtTranslator;
	if (qtTranslator.load(QLocale(), ":/translations/qtbase", "_"))
		app.installTranslator(&qtTranslator);

	QTranslator deviceSelectorTranslator;
	if (deviceSelectorTranslator.load(
		QLocale(), ":/translations/DeviceSelector", "_"))
		app.installTranslator(&deviceSelectorTranslator);
	const bool silentMode = app.arguments().contains("/s", Qt::CaseInsensitive);

	if (app.arguments().contains("/r", Qt::CaseInsensitive))
	{
		try
		{
			ServiceHelper::restartService(L"AudioSrv");
			return 0;
		}
		catch (ServiceException&)
		{
			return 1;
		}
	}

	if (app.arguments().contains("/u", Qt::CaseInsensitive))
	{
		for (int index = 0; index <= 1; index++)
		{
			std::vector<std::shared_ptr<AbstractAPOInfo>> apoInfos =
				DeviceAPOInfo::loadAllInfos(index == 1);

			for (std::shared_ptr<AbstractAPOInfo>& apoInfo : apoInfos)
			{
				try
				{
					if (apoInfo->isInstalled())
						apoInfo->uninstall();
				}
				catch (const RegistryException& e)
				{
					if (!silentMode)
					{
						QMessageBox::critical(nullptr,
							DeviceSelector::tr(
								"Error while accessing the registry"),
							QString::fromStdWString(e.getMessage()));
					}
					result = -1;
				}
			}
		}

		VoicemeeterAPOInfo::ensureVoicemeeterClientRunning();
	}
	else
	{
		DeviceSelector dialog;
		UiSnapshot::prepareForCapture(dialog);
		dialog.show();
		UiSnapshot::schedule(dialog, app);
		result = app.exec();
	}

	return result;
}
