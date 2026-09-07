/*
	This file is part of Equalizer APO, a system-wide equalizer.
	Copyright (C) 2026  Equalizer APO contributors

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
*/

#pragma once

#include <functional>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

namespace UiSnapshot
{
#ifdef EQAPO_ENABLE_UI_SNAPSHOTS
	inline QString outputPath()
	{
		return qEnvironmentVariable("EQAPO_UI_SNAPSHOT").trimmed();
	}

	inline bool requested()
	{
		return !outputPath().isEmpty();
	}

	inline QString scenario()
	{
		const QString value = qEnvironmentVariable(
			"EQAPO_UI_SNAPSHOT_SCENARIO").trimmed().toLower();
		return value == QStringLiteral("dense")
			|| value == QStringLiteral("volume-follow")
			|| value == QStringLiteral("restored-tools")
			? value : QString();
	}

	inline QString localeName()
	{
		const QString value = qEnvironmentVariable(
			"EQAPO_UI_SNAPSHOT_LOCALE").trimmed();
		if (value == QStringLiteral("de") || value == QStringLiteral("fr")
			|| value == QStringLiteral("zh_CN") || value == QStringLiteral("zh_TW"))
			return value;
		return QStringLiteral("en");
	}

	inline void prepareForCapture(QWidget& widget)
	{
		if (requested())
			widget.setAttribute(Qt::WA_DontShowOnScreen, true);
	}

	inline bool environmentIsValid(const QWidget& widget)
	{
		if (QGuiApplication::platformName() != QStringLiteral("windows"))
			return false;

		const QFontMetrics metrics(widget.font());
		if (!metrics.inFontUcs4('A') || !metrics.inFontUcs4('0'))
			return false;
		return !localeName().startsWith(QStringLiteral("zh_"))
			|| metrics.inFontUcs4(0x6e2c);
	}

	inline void schedule(
		QWidget& widget,
		QApplication& application,
		const std::function<bool()>& validator = std::function<bool()>())
	{
		const QString path = outputPath();
		if (path.isEmpty())
			return;

		bool validDelay = false;
		const int requestedDelay = qEnvironmentVariableIntValue(
			"EQAPO_UI_SNAPSHOT_DELAY_MS", &validDelay);
		const int delay = validDelay ? qBound(0, requestedDelay, 10000) : 650;

		QTimer::singleShot(delay, &widget,
			[&widget, &application, path, validator]
		{
			if (!environmentIsValid(widget))
			{
				application.exit(88);
				return;
			}
			if (validator && !validator())
			{
				application.exit(87);
				return;
			}
			const QFileInfo target(path);
			const bool directoryReady = QDir().mkpath(target.absolutePath());
			const bool saved = directoryReady && widget.grab().save(path, "PNG");
			application.exit(saved ? 0 : 86);
		});
	}
#else
	inline QString outputPath()
	{
		return QString();
	}

	inline bool requested()
	{
		return false;
	}

	inline QString scenario()
	{
		return QString();
	}

	inline QString localeName()
	{
		return QString();
	}

	inline void prepareForCapture(QWidget& widget)
	{
		Q_UNUSED(widget);
	}

	inline bool environmentIsValid(const QWidget& widget)
	{
		Q_UNUSED(widget);
		return true;
	}

	inline void schedule(
		QWidget& widget,
		QApplication& application,
		const std::function<bool()>& validator = std::function<bool()>())
	{
		Q_UNUSED(widget);
		Q_UNUSED(application);
		Q_UNUSED(validator);
	}
#endif
}
