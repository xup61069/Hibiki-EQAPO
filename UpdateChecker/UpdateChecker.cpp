/*
	This file is part of EqualizerAPO, a system-wide equalizer.
	Copyright (C) 2024  Jonas Dahlinger

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
#include "UpdateChecker.h"

UpdateChecker::UpdateChecker(QWidget* parent, const QString& installedVersion)
	: QDialog(parent), installedVersion(installedVersion)
{
	ui.setupUi(this);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);

	ui.installedVersionLabel->setText(installedVersion);
	ui.statusProgressBar->setRange(0, 0);
	ui.statusProgressBar->setTextVisible(false);
	ui.sectionLabel->setText(QStringLiteral("Hibiki EQAPO"));
	ui.installedCaptionLabel->setText(tr("Installed version"));
	ui.availableCaptionLabel->setText(tr("Available version"));
	ui.releaseNotesLabel->setText(tr("What's new"));
	ui.skipButton->setText(tr("Skip this version"));
	ui.retryButton->setText(tr("Try again"));
	ui.goButton->setText(tr("Open download page"));

	ui.goButton->setAccessibleName(tr("Open download page"));
	ui.goButton->setAccessibleDescription(tr("Opens the verified HTTPS release page in your default browser."));
	ui.retryButton->setAccessibleName(tr("Retry update check"));
	ui.retryButton->setAccessibleDescription(tr("Checks GitHub Releases again."));
	ui.laterButton->setAccessibleName(tr("Close update checker"));
	ui.laterButton->setAccessibleDescription(tr("Closes the update checker without changing the skipped version."));
	ui.skipButton->setAccessibleName(tr("Skip this version"));
	ui.skipButton->setAccessibleDescription(tr("Hides this release during future automatic checks."));
	ui.versionBrowser->setAccessibleName(tr("Release notes"));
	ui.versionBrowser->setAccessibleDescription(tr("Release notes for the available version."));
	ui.statusProgressBar->setAccessibleName(tr("Update check progress"));

	connect(ui.goButton, &QPushButton::clicked, this, &UpdateChecker::goToWebsite);
	connect(ui.laterButton, &QPushButton::clicked, this, &UpdateChecker::remindMeLater);
	connect(ui.skipButton, &QPushButton::clicked, this, &UpdateChecker::skipThisVersion);
	connect(ui.retryButton, &QPushButton::clicked, this, [this] {
		emit retryRequested();
	});

	showChecking();
}

UpdateChecker::~UpdateChecker()
{
}

bool UpdateChecker::snapshotLayoutIsValid() const
{
#ifdef EQAPO_ENABLE_UI_SNAPSHOTS
	const QList<QWidget*> heightSensitiveWidgets{
		ui.sectionLabel,
		ui.titleLabel,
		ui.installedCaptionLabel,
		ui.installedVersionLabel,
		ui.availableCaptionLabel,
		ui.availableVersionLabel,
		ui.statusProgressBar,
		ui.releaseNotesLabel,
		ui.skipButton,
		ui.laterButton,
		ui.retryButton,
		ui.goButton
	};
	for (QWidget* widget : heightSensitiveWidgets)
	{
		if (!widget->isVisible())
			continue;
		const QRect geometryInDialog(widget->mapTo(this, QPoint(0, 0)), widget->size());
		if (widget->height() < widget->sizeHint().height()
			|| !rect().contains(geometryInDialog))
		{
			qWarning() << "Snapshot layout clipped" << widget->objectName()
				<< "geometry" << geometryInDialog << "size hint" << widget->sizeHint();
			return false;
		}
	}
#endif
	return true;
}

void UpdateChecker::showChecking()
{
	downloadUrl.clear();
	newestVersion.clear();
	applyState(State::Checking);
}

void UpdateChecker::showUpToDate(const QString& availableVersion)
{
	newestVersion = availableVersion.isEmpty() ? installedVersion : availableVersion;
	applyState(State::UpToDate);
}

void UpdateChecker::showUpdateAvailable(const QJsonDocument& doc)
{
	populateReleaseNotes(doc);
	applyState(State::UpdateAvailable);
}

void UpdateChecker::showFailure(const QString& message)
{
	ui.statusMessageLabel->setText(message.trimmed().isEmpty()
		? tr("The update service did not provide an error message.")
		: message.trimmed());
	applyState(State::Failure);
}

void UpdateChecker::applyState(State newState)
{
	state = newState;

	const bool isChecking = state == State::Checking;
	const bool isAvailable = state == State::UpdateAvailable;
	const bool isFailure = state == State::Failure;
	const bool isUpToDate = state == State::UpToDate;

	ui.statusProgressBar->setVisible(isChecking);
	ui.releaseNotesLabel->setVisible(isAvailable);
	ui.versionBrowser->setVisible(isAvailable);
	ui.goButton->setVisible(isAvailable);
	ui.skipButton->setVisible(isAvailable);
	ui.retryButton->setVisible(isFailure);
	ui.statusMessageLabel->setVisible(!isAvailable);
	ui.availableCaptionLabel->setVisible(!isChecking || isAvailable);
	ui.availableVersionLabel->setVisible(!isChecking || isAvailable);

	ui.goButton->setDefault(false);
	ui.retryButton->setDefault(false);
	ui.laterButton->setDefault(false);
	ui.goButton->setAutoDefault(false);
	ui.retryButton->setAutoDefault(false);
	ui.laterButton->setAutoDefault(false);

	if (isChecking)
	{
		setWindowTitle(QStringLiteral("Hibiki EQAPO — ") + tr("Checking for updates"));
		ui.titleLabel->setText(tr("Checking for updates"));
		ui.summaryLabel->setText(tr("Connecting securely to GitHub Releases."));
		ui.availableVersionLabel->setText(tr("Checking…"));
		ui.statusMessageLabel->setText(tr("This usually takes only a few seconds."));
		ui.laterButton->setText(tr("Cancel"));
		ui.laterButton->setFocus(Qt::OtherFocusReason);
		refreshStatusProperty(nullptr);
	}
	else if (isUpToDate)
	{
		setWindowTitle(QStringLiteral("Hibiki EQAPO — ") + tr("No update available"));
		ui.titleLabel->setText(tr("You're up to date"));
		ui.summaryLabel->setText(tr("Version %1 is the latest available release.").arg(newestVersion));
		ui.availableVersionLabel->setText(newestVersion);
		ui.statusMessageLabel->setText(tr("No action is needed."));
		ui.laterButton->setText(tr("Close"));
		ui.laterButton->setDefault(true);
		ui.laterButton->setAutoDefault(true);
		ui.laterButton->setFocus(Qt::OtherFocusReason);
		refreshStatusProperty(nullptr);
	}
	else if (isAvailable)
	{
		setWindowTitle(QStringLiteral("Hibiki EQAPO — ") + tr("Update available"));
		ui.titleLabel->setText(tr("Version %1 is available").arg(newestVersion));
		ui.summaryLabel->setText(tr("Review what changed, then open the secure download page."));
		ui.availableVersionLabel->setText(newestVersion);
		ui.laterButton->setText(tr("Remind me later"));
		ui.goButton->setEnabled(downloadUrl.isValid() && downloadUrl.scheme() == QStringLiteral("https"));
		ui.goButton->setDefault(ui.goButton->isEnabled());
		ui.goButton->setAutoDefault(ui.goButton->isEnabled());
		(ui.goButton->isEnabled() ? ui.goButton : ui.laterButton)->setFocus(Qt::OtherFocusReason);
		refreshStatusProperty(nullptr);
	}
	else
	{
		setWindowTitle(QStringLiteral("Hibiki EQAPO — ") + tr("Update check failed"));
		ui.titleLabel->setText(tr("We couldn't check for updates"));
		ui.summaryLabel->setText(tr("Check your internet connection, then try again."));
		ui.availableVersionLabel->setText(tr("Unavailable"));
		ui.laterButton->setText(tr("Close"));
		ui.retryButton->setDefault(true);
		ui.retryButton->setAutoDefault(true);
		ui.retryButton->setFocus(Qt::OtherFocusReason);
		refreshStatusProperty("danger");
	}

	const int preferredHeight = isAvailable ? 530 : 380;
	const int baseMinimumHeight = isAvailable ? 460 : 340;
	setMinimumHeight(baseMinimumHeight);
	layout()->activate();
	const int contentMinimumHeight = layout()->minimumSize().height();
	const int targetHeight = qMax(preferredHeight, contentMinimumHeight);
	setMinimumHeight(qMax(baseMinimumHeight, contentMinimumHeight));
	if (!isVisible() || height() <= 530)
		resize(qMax(width(), 560), targetHeight);

	refreshStatusIcon();
	ui.headerPanel->updateGeometry();
	ui.contentPanel->updateGeometry();
}

void UpdateChecker::populateReleaseNotes(const QJsonDocument& doc)
{
	downloadUrl.clear();
	newestVersion.clear();

	QJsonObject docObj = doc.object();
	downloadUrl = QUrl(docObj.value("download-url").toString(), QUrl::StrictMode);

	QJsonArray versionsArray = docObj.value("versions").toArray();
	QString html = "<style>\n.date{font-size:small;font-style:italic;}\nul{margin:8px 0 14px 22px;}\nli{margin:4px 0;}\n</style>\n";
	bool first = true;
	bool hasReleaseNotes = false;
	for (QJsonValue versionValue : versionsArray)
	{
		QJsonObject versionObj = versionValue.toObject();
		QString version = versionObj.value("version").toString();
		if (first)
		{
			newestVersion = version;
			first = false;
		}
		QDate date = QDate::fromString(versionObj.value("date").toString(), Qt::ISODate);
		html.append(QString("<div><b>%1 </b><span class=\"date\">(%2)</span></div>")
			.arg(version.toHtmlEscaped()).arg(QLocale().toString(date, QLocale::ShortFormat).toHtmlEscaped()));
		html.append("<ul>");
		QJsonArray infoArray = versionObj.value("info").toArray();
		for (QJsonValue v : infoArray)
		{
			html.append("<li>" + v.toString().toHtmlEscaped() + "</li>");
			hasReleaseNotes = true;
		}
		html.append("</ul>");
	}
	if (!hasReleaseNotes)
		html.append(QStringLiteral("<p>%1</p>").arg(tr("No release notes were provided.").toHtmlEscaped()));

	ui.versionBrowser->setHtml(html);
	ui.versionBrowser->document()->adjustSize();
}

void UpdateChecker::refreshStatusIcon()
{
	const qreal pixelRatio = devicePixelRatioF();
	const QSize logicalSize(44, 44);
	QPixmap pixmap(logicalSize * pixelRatio);
	pixmap.setDevicePixelRatio(pixelRatio);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing, false);
	const QColor accent = palette().color(QPalette::Highlight);
	const QColor foreground = palette().color(QPalette::HighlightedText);
	const QRectF box(1.5, 1.5, 41.0, 41.0);
	painter.fillRect(box, accent);
	painter.setPen(QPen(foreground, 2.4, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));

	if (state == State::UpToDate)
	{
		QPainterPath check;
		check.moveTo(12, 23);
		check.lineTo(19, 30);
		check.lineTo(33, 14);
		painter.drawPath(check);
	}
	else if (state == State::Failure)
	{
		painter.drawLine(QPointF(22, 11), QPointF(22, 27));
		painter.drawPoint(QPointF(22, 34));
	}
	else
	{
		QPainterPath arrow;
		arrow.moveTo(22, 10);
		arrow.lineTo(22, 30);
		arrow.moveTo(14, 22);
		arrow.lineTo(22, 30);
		arrow.lineTo(30, 22);
		painter.drawPath(arrow);
		painter.drawLine(QPointF(12, 35), QPointF(32, 35));
	}

	ui.statusIconLabel->setPixmap(pixmap);
	ui.statusIconLabel->setAccessibleName(
		state == State::Checking ? tr("Checking for updates")
		: state == State::UpToDate ? tr("Up to date")
		: state == State::UpdateAvailable ? tr("Update available")
		: tr("Update check failed"));
}

void UpdateChecker::refreshStatusProperty(const char* level)
{
	ui.statusMessageLabel->setProperty("statusLevel", level ? QVariant(level) : QVariant());
	ui.statusMessageLabel->style()->unpolish(ui.statusMessageLabel);
	ui.statusMessageLabel->style()->polish(ui.statusMessageLabel);
}

void UpdateChecker::changeEvent(QEvent* event)
{
	QDialog::changeEvent(event);
	if (event->type() == QEvent::PaletteChange
		|| event->type() == QEvent::ApplicationPaletteChange
		|| event->type() == QEvent::ThemeChange)
		refreshStatusIcon();
}

void UpdateChecker::goToWebsite()
{
	if (!downloadUrl.isValid() || downloadUrl.scheme() != "https")
		return;
	QDesktopServices::openUrl(downloadUrl);

	QSettings settings(QString::fromWCharArray(UPDATE_CHECKER_REGPATH), QSettings::NativeFormat);
	settings.remove("skipVersion");
	accept();
}

void UpdateChecker::remindMeLater()
{
	if (state == State::UpdateAvailable)
	{
		QSettings settings(QString::fromWCharArray(UPDATE_CHECKER_REGPATH), QSettings::NativeFormat);
		settings.remove("skipVersion");
	}
	reject();
}

void UpdateChecker::skipThisVersion()
{
	QSettings settings(QString::fromWCharArray(UPDATE_CHECKER_REGPATH), QSettings::NativeFormat);
	settings.setValue("skipVersion", newestVersion);
	reject();
}
