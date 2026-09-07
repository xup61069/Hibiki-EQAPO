/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026  Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "LoudnessCorrectionStudioDialog.h"
#include "ui_LoudnessCorrectionStudioDialog.h"

#include "Editor/helpers/GUIHelper.h"
#include "filters/loudnessCorrection/LoudnessProfile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif
#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QScreen>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleHints>

namespace
{
	QString signedNumber(int value)
	{
		return value > 0 ? QString("+%1").arg(value) : QString::number(value);
	}

	QColor blendColors(const QColor& background, const QColor& foreground, qreal amount)
	{
		const qreal boundedAmount = qBound(0.0, amount, 1.0);
		return QColor(
			qRound(background.red() + (foreground.red() - background.red()) * boundedAmount),
			qRound(background.green() + (foreground.green() - background.green()) * boundedAmount),
			qRound(background.blue() + (foreground.blue() - background.blue()) * boundedAmount));
	}

	QString styleColor(const QColor& color)
	{
		return color.name(QColor::HexRgb);
	}

	bool isHighContrastEnabled()
	{
		if (qApp != nullptr &&
			qApp->property("eqapoModernThemeHighContrast").toBool())
			return true;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
		const QStyleHints* styleHints = QGuiApplication::styleHints();
		const QAccessibilityHints* accessibility =
			styleHints != nullptr ? styleHints->accessibility() : nullptr;
		return accessibility != nullptr &&
			accessibility->contrastPreference() == Qt::ContrastPreference::HighContrast;
#else
		return false;
#endif
	}
}

LoudnessCorrectionStudioDialog::LoudnessCorrectionStudioDialog(
	int referenceLevel,
	int referenceOffset,
	double attenuation,
	bool globalBinding,
	bool useManualVolume,
	double volume,
	bool automaticVolumeAvailable,
	LoudnessCorrectionFilter::FilterParameters::VolumeFollowMode volumeFollow,
	double automaticVolumeScalar,
	double automaticVolumeDb,
	QWidget* parent)
	: QDialog(parent),
	  ui(new Ui::LoudnessCorrectionStudioDialog),
	  initialGlobalBinding(globalBinding),
	  automaticVolumeAvailable(automaticVolumeAvailable),
	  volumeFollow(volumeFollow),
	  automaticVolumeScalar(automaticVolumeScalar),
	  automaticVolumeDb(automaticVolumeDb),
	  calibrateAfterApply(false),
	  applyingModernStyle(false)
{
	ui->setupUi(this);
	setAttribute(Qt::WA_StyledBackground, true);
	QSize minimumDialogSize = GUIHelper::scale(QSize(520, 480));
	QSize preferredSize = GUIHelper::scale(QSize(1040, 640));
	QScreen* availableScreen = parentWidget() != nullptr
		? parentWidget()->screen() : QGuiApplication::screenAt(frameGeometry().center());
	if (availableScreen == nullptr)
		availableScreen = QGuiApplication::primaryScreen();
	if (availableScreen != nullptr)
	{
		const QSize screenMargin = GUIHelper::scale(QSize(32, 32));
		const QSize availableSize =
			availableScreen->availableGeometry().size() - screenMargin;
		minimumDialogSize = minimumDialogSize.boundedTo(availableSize);
		preferredSize = preferredSize.boundedTo(availableSize);
	}
	setMinimumSize(minimumDialogSize);
	resize(preferredSize.expandedTo(minimumSize()));

	{
		QSignalBlocker referenceSliderBlocker(ui->referenceSlider);
		QSignalBlocker referenceSpinBlocker(ui->refLevelSpinBox);
		QSignalBlocker offsetSliderBlocker(ui->offsetSlider);
		QSignalBlocker offsetSpinBlocker(ui->refOffsetSpinBox);
		QSignalBlocker strengthSliderBlocker(ui->strengthSlider);
		QSignalBlocker strengthSpinBlocker(ui->strengthSpinBox);
		QSignalBlocker bindingBlocker(ui->bindingComboBox);
		QSignalBlocker manualBlocker(ui->manualVolumeCheckBox);
		QSignalBlocker volumeBlocker(ui->volumeSpinBox);

		ui->referenceSlider->setValue(referenceLevel);
		ui->refLevelSpinBox->setValue(referenceLevel);
		ui->offsetSlider->setValue(referenceOffset);
		ui->refOffsetSpinBox->setValue(referenceOffset);
		ui->strengthSlider->setValue(qRound(attenuation * 100.0));
		ui->strengthSpinBox->setValue(attenuation);
		ui->bindingComboBox->setCurrentIndex(globalBinding ? 1 : 0);
		ui->manualVolumeCheckBox->setChecked(useManualVolume);
		ui->volumeSpinBox->setValue(volume);
	}

	const int footerButtonHeight = GUIHelper::scale(40);
	ui->resetButton->setFixedHeight(footerButtonHeight);
	ui->cancelButton->setFixedHeight(footerButtonHeight);
	ui->applyButton->setFixedHeight(footerButtonHeight);

	ui->curvePreview->installEventFilter(this);
	updateResponsiveLayout();
	applyModernStyle();
	updateModernUi();
}

LoudnessCorrectionStudioDialog::~LoudnessCorrectionStudioDialog()
{
	delete ui;
}

int LoudnessCorrectionStudioDialog::getReferenceLevel() const
{
	return ui->refLevelSpinBox->value();
}

int LoudnessCorrectionStudioDialog::getReferenceOffset() const
{
	return ui->refOffsetSpinBox->value();
}

double LoudnessCorrectionStudioDialog::getAttenuation() const
{
	return ui->strengthSpinBox->value();
}

bool LoudnessCorrectionStudioDialog::getGlobalBinding() const
{
	return ui->bindingComboBox->currentIndex() == 1;
}

bool LoudnessCorrectionStudioDialog::getUseManualVolume() const
{
	return ui->manualVolumeCheckBox->isChecked();
}

double LoudnessCorrectionStudioDialog::getVolume() const
{
	return ui->volumeSpinBox->value();
}

bool LoudnessCorrectionStudioDialog::shouldCalibrateAfterApply() const
{
	return calibrateAfterApply;
}

void LoudnessCorrectionStudioDialog::changeEvent(QEvent* event)
{
	QDialog::changeEvent(event);
	if (applyingModernStyle)
		return;

	if (event->type() == QEvent::ApplicationPaletteChange ||
		event->type() == QEvent::PaletteChange ||
		event->type() == QEvent::ThemeChange)
	{
		applyModernStyle();
		ui->curvePreview->update();
	}
}

bool LoudnessCorrectionStudioDialog::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == ui->curvePreview && event->type() == QEvent::Paint)
	{
		QPainter painter(ui->curvePreview);
		paintCurvePreview(painter);
		return true;
	}
	return QDialog::eventFilter(watched, event);
}

void LoudnessCorrectionStudioDialog::resizeEvent(QResizeEvent* event)
{
	QDialog::resizeEvent(event);
	updateResponsiveLayout();
}

void LoudnessCorrectionStudioDialog::updateResponsiveLayout()
{
	const bool useCompactLayout = width() < GUIHelper::scale(820);
	if (responsiveLayoutInitialized && compactLayout == useCompactLayout)
		return;

	responsiveLayoutInitialized = true;
	compactLayout = useCompactLayout;
	ui->contentLayout->removeWidget(ui->curveCard);
	ui->contentLayout->removeWidget(ui->volumeCard);
	ui->contentLayout->removeWidget(ui->parametersCard);

	if (compactLayout)
	{
		ui->contentLayout->addWidget(ui->curveCard, 0, 0);
		ui->contentLayout->addWidget(ui->volumeCard, 1, 0);
		ui->contentLayout->addWidget(ui->parametersCard, 2, 0);
		ui->contentLayout->setColumnStretch(0, 1);
		ui->contentLayout->setColumnStretch(1, 0);

		ui->parametersLayout->setDirection(QBoxLayout::TopToBottom);
		ui->parameterDivider1->setFrameShape(QFrame::HLine);
		ui->parameterDivider2->setFrameShape(QFrame::HLine);
		ui->heroLayout->setDirection(QBoxLayout::TopToBottom);
		ui->curveHeaderLayout->setDirection(QBoxLayout::TopToBottom);
		ui->footerLayout->setDirection(QBoxLayout::TopToBottom);
		ui->profileSummaryLabel->setAlignment(
			Qt::AlignLeft | Qt::AlignVCenter);
		ui->curveMetaLabel->setAlignment(
			Qt::AlignLeft | Qt::AlignVCenter);
	}
	else
	{
		ui->contentLayout->addWidget(ui->curveCard, 0, 0);
		ui->contentLayout->addWidget(ui->volumeCard, 0, 1);
		ui->contentLayout->addWidget(ui->parametersCard, 1, 0, 1, 2);
		ui->contentLayout->setColumnStretch(0, 3);
		ui->contentLayout->setColumnStretch(1, 2);

		ui->parametersLayout->setDirection(QBoxLayout::LeftToRight);
		ui->parameterDivider1->setFrameShape(QFrame::VLine);
		ui->parameterDivider2->setFrameShape(QFrame::VLine);
		ui->heroLayout->setDirection(QBoxLayout::LeftToRight);
		ui->curveHeaderLayout->setDirection(QBoxLayout::LeftToRight);
		ui->footerLayout->setDirection(QBoxLayout::LeftToRight);
		ui->profileSummaryLabel->setAlignment(
			Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
		ui->curveMetaLabel->setAlignment(
			Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
	}

	ui->contentScrollWidget->setMinimumWidth(0);
	ui->contentScrollWidget->updateGeometry();
}

void LoudnessCorrectionStudioDialog::on_referenceSlider_valueChanged(int value)
{
	ui->refLevelSpinBox->setValue(value);
}

void LoudnessCorrectionStudioDialog::on_refLevelSpinBox_valueChanged(int value)
{
	ui->referenceSlider->setValue(value);
	updateModernUi();
}

void LoudnessCorrectionStudioDialog::on_offsetSlider_valueChanged(int value)
{
	ui->refOffsetSpinBox->setValue(value);
}

void LoudnessCorrectionStudioDialog::on_refOffsetSpinBox_valueChanged(int value)
{
	ui->offsetSlider->setValue(value);
	updateModernUi();
}

void LoudnessCorrectionStudioDialog::on_strengthSlider_valueChanged(int value)
{
	ui->strengthSpinBox->setValue(value / 100.0);
}

void LoudnessCorrectionStudioDialog::on_strengthSpinBox_valueChanged(double value)
{
	ui->strengthSlider->setValue(qRound(value * 100.0));
	updateModernUi();
}

void LoudnessCorrectionStudioDialog::on_bindingComboBox_currentIndexChanged(int index)
{
	Q_UNUSED(index);
	updateModernUi();
}

void LoudnessCorrectionStudioDialog::on_manualVolumeCheckBox_toggled(bool checked)
{
	if (!checked && automaticVolumeAvailable)
	{
		QSignalBlocker blocker(ui->volumeSpinBox);
		ui->volumeSpinBox->setValue(automaticVolumeDb);
	}
	updateModernUi();
}

void LoudnessCorrectionStudioDialog::on_volumeSpinBox_valueChanged(double value)
{
	Q_UNUSED(value);
	updateModernUi();
}

void LoudnessCorrectionStudioDialog::on_resetButton_clicked()
{
	ui->refLevelSpinBox->setValue(80);
	ui->refOffsetSpinBox->setValue(0);
	ui->strengthSpinBox->setValue(1.0);
	updateModernUi();
}

void LoudnessCorrectionStudioDialog::on_calibrateButton_clicked()
{
	calibrateAfterApply = true;
	accept();
}

void LoudnessCorrectionStudioDialog::applyModernStyle()
{
	if (applyingModernStyle)
		return;

	QScopedValueRollback<bool> styleUpdateGuard(applyingModernStyle, true);
	const QString geometryStyle = QStringLiteral(R"QSS(
QDialog#LoudnessCorrectionStudioDialog {
	font-size: 10pt;
}
QLabel#brandMark {
	font-size: 14pt;
	font-weight: 700;
}
QLabel#titleLabel {
	font-size: 21pt;
	font-weight: 650;
}
QLabel#curveTitleLabel, QLabel#volumeTitleLabel {
	font-size: 12pt;
	font-weight: 650;
}
QLabel#referenceTitleLabel, QLabel#offsetTitleLabel, QLabel#strengthTitleLabel {
	font-weight: 600;
}
QLabel#creditLabel, QLabel#curveDescriptionLabel, QLabel#referenceHintLabel,
QLabel#offsetHintLabel, QLabel#strengthHintLabel, QLabel#footerHintLabel {
	font-size: 9pt;
}
QLabel#profileSummaryLabel, QLabel#curveMetaLabel {
	font-family: "Consolas";
	font-size: 9pt;
}
QLabel#trackingStatusLabel {
	padding: 3px 10px;
	font-weight: 600;
}
QLabel#trackingStatusLabel[status="pending"] {
	border-style: dashed;
}
QComboBox::drop-down {
	border: none;
	width: 26px;
}
QComboBox, QAbstractSpinBox {
	max-height: 16777215px;
}
QCheckBox {
	spacing: 9px;
}
QSlider::groove:horizontal {
	height: 6px;
}
QSlider::handle:horizontal {
	width: 16px;
	height: 16px;
	margin: -5px 0;
	border-radius: 0;
}
QPushButton {
	min-height: 40px;
	max-height: 40px;
	padding: 0 14px;
	font-weight: 600;
}
QPushButton#resetButton {
	text-align: left;
}
)QSS");

	if (isHighContrastEnabled())
	{
		if (styleSheet() != geometryStyle)
			setStyleSheet(geometryStyle);
		return;
	}

	const QPalette applicationPalette = QApplication::palette();
	const QColor canvas = applicationPalette.color(QPalette::Window);
	const QColor surface = applicationPalette.color(QPalette::Base);
	const QColor raised = applicationPalette.color(QPalette::AlternateBase);
	const QColor border = applicationPalette.color(QPalette::Mid);
	const QColor text = applicationPalette.color(QPalette::WindowText);
	const QColor muted = applicationPalette.color(QPalette::PlaceholderText);
	const QColor accent = applicationPalette.color(QPalette::Highlight);
	const QColor accentText = applicationPalette.color(QPalette::HighlightedText);
	const QColor accentSoft = blendColors(surface, accent, 0.16);
	const QColor accentHover = blendColors(accent, accentText, 0.13);

	QString themedStyle = geometryStyle + QStringLiteral(R"QSS(
QDialog#LoudnessCorrectionStudioDialog {
	background: @canvas;
	color: @text;
}
QFrame#heroFrame {
	background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 @surface, stop:1 @raised);
	border: 1px solid @border;
}
QFrame#curveCard, QFrame#volumeCard, QFrame#parametersCard {
	background: @surface;
	border: 1px solid @border;
}
QLabel#brandMark {
	background: @accent;
	color: @accentText;
	border: none;
}
QLabel#titleLabel, QLabel#curveTitleLabel, QLabel#volumeTitleLabel,
QLabel#referenceTitleLabel, QLabel#offsetTitleLabel, QLabel#strengthTitleLabel {
	color: @text;
}
QLabel#subtitleLabel, QLabel#creditLabel, QLabel#curveDescriptionLabel,
QLabel#referenceHintLabel, QLabel#offsetHintLabel, QLabel#strengthHintLabel,
QLabel#volumeHintLabel, QLabel#footerHintLabel, QLabel#bindingLabel,
QLabel#profileSummaryLabel, QLabel#curveMetaLabel {
	color: @muted;
}
QLabel#trackingStatusLabel {
	border: 1px solid @border;
}
QLabel#trackingStatusLabel[status="automatic"] {
	background: @accentSoft;
	border-color: @accent;
	color: @accent;
}
QLabel#trackingStatusLabel[status="manual"] {
	background: @raised;
	color: @text;
}
QLabel#trackingStatusLabel[status="pending"] {
	background: @raised;
	color: @muted;
}
QLabel#trackingStatusLabel[status="required"] {
	background: @raised;
	border-color: @muted;
	color: @text;
}
QComboBox, QAbstractSpinBox {
	background: @raised;
	color: @text;
	border: 1px solid @border;
	padding: 5px 10px;
	selection-background-color: @accent;
	selection-color: @accentText;
}
QComboBox:hover, QAbstractSpinBox:hover,
QComboBox:focus, QAbstractSpinBox:focus {
	border-color: @accent;
}
QDoubleSpinBox[readOnly="true"] {
	background: @surface;
	color: @text;
}
QCheckBox {
	color: @text;
}
QSlider::groove:horizontal {
	background: @border;
}
QSlider::sub-page:horizontal {
	background: @accent;
}
QSlider::handle:horizontal {
	background: @surface;
	border: 3px solid @accent;
}
QSlider::handle:horizontal:hover {
	background: @accent;
}
QPushButton {
	background: @raised;
	color: @text;
	border: 1px solid @border;
}
QPushButton:hover, QPushButton:focus {
	border-color: @accent;
	color: @accent;
}
QPushButton:pressed {
	background: @accentSoft;
}
QPushButton#applyButton, QPushButton#calibrateButton {
	background: @accent;
	color: @accentText;
	border-color: @accent;
}
QPushButton#applyButton:hover, QPushButton#calibrateButton:hover,
QPushButton#applyButton:focus, QPushButton#calibrateButton:focus {
	background: @accentHover;
	color: @accentText;
	border-color: @accentHover;
}
QPushButton#resetButton {
	background: transparent;
	color: @muted;
	border-color: transparent;
}
QPushButton#resetButton:hover, QPushButton#resetButton:focus {
	color: @accent;
	border-color: transparent;
}
QFrame#parameterDivider1, QFrame#parameterDivider2 {
	color: @border;
}
QToolTip {
	background: @raised;
	color: @text;
	border: 1px solid @border;
	padding: 6px;
}
)QSS");

	themedStyle.replace("@accentText", styleColor(accentText));
	themedStyle.replace("@accentHover", styleColor(accentHover));
	themedStyle.replace("@accentSoft", styleColor(accentSoft));
	themedStyle.replace("@accent", styleColor(accent));
	themedStyle.replace("@canvas", styleColor(canvas));
	themedStyle.replace("@surface", styleColor(surface));
	themedStyle.replace("@raised", styleColor(raised));
	themedStyle.replace("@border", styleColor(border));
	themedStyle.replace("@muted", styleColor(muted));
	themedStyle.replace("@text", styleColor(text));

	if (styleSheet() != themedStyle)
		setStyleSheet(themedStyle);
}

double LoudnessCorrectionStudioDialog::listeningVolumeDb() const
{
	const bool manual = getUseManualVolume();
	if (!manual && (!automaticVolumeAvailable || getGlobalBinding() != initialGlobalBinding))
		return std::numeric_limits<double>::quiet_NaN();
	const double volume = getVolume();
	return LoudnessCorrectionFilter::calculateListeningVolumeDb(
		volumeFollow, volume, manual ? (volume + 100.0) / 100.0 : automaticVolumeScalar);
}

void LoudnessCorrectionStudioDialog::updateModernUi()
{
	const int referenceLevel = ui->refLevelSpinBox->value();
	const int referenceOffset = ui->refOffsetSpinBox->value();
	const int strengthPercent = qRound(ui->strengthSpinBox->value() * 100.0);
	const double currentLevel = (std::max)(0.0, (std::min)(100.0,
		referenceLevel + listeningVolumeDb() - referenceOffset));

	ui->profileSummaryLabel->setText(tr("%1 phon · %2 dB · %3%")
		.arg(referenceLevel)
		.arg(signedNumber(referenceOffset))
		.arg(strengthPercent));
	ui->curveMetaLabel->setText(tr("Estimated %1 phon · %2% strength")
		.arg(QString::number(currentLevel, 'f', 0))
		.arg(strengthPercent));
	if (!std::isfinite(listeningVolumeDb()))
		ui->curveMetaLabel->setText(tr("Check after apply"));

	const bool manual = ui->manualVolumeCheckBox->isChecked();
	const bool bindingChanged = getGlobalBinding() != initialGlobalBinding;
	ui->volumeSpinBox->setReadOnly(!manual);

	if (manual)
	{
		ui->trackingStatusLabel->setText(tr("Manual · %1 dB")
			.arg(QString::number(ui->volumeSpinBox->value(), 'f', 1)));
		ui->volumeHintLabel->setText(volumeFollow ==
			LoudnessCorrectionFilter::FilterParameters::VOLUME_FOLLOW_OFF ? tr(
			"Use the same hardware volume position represented by this value.") :
			tr("APO follow target: %1 dB").arg(listeningVolumeDb(), 0, 'f', 2));
		refreshStatusStyle("manual");
	}
	else if (bindingChanged)
	{
		ui->trackingStatusLabel->setText(tr("Check after apply"));
		ui->volumeHintLabel->setText(tr(
			"The new binding will be verified when these changes are applied."));
		refreshStatusStyle("pending");
	}
	else if (automaticVolumeAvailable)
	{
		ui->trackingStatusLabel->setText(tr("Automatic · %1 dB")
			.arg(QString::number(ui->volumeSpinBox->value(), 'f', 1)));
		ui->volumeHintLabel->setText(tr("Preview uses the volume snapshot from when this window opened."));
		refreshStatusStyle("automatic");
	}
	else
	{
		ui->trackingStatusLabel->setText(tr("Automatic volume unavailable · paused"));
		ui->volumeHintLabel->setText(tr(
			"Automatic mode is preserved. Runtime correction resumes when the endpoint volume is readable again."));
		refreshStatusStyle("required");
	}
	ui->trackingStatusLabel->setAccessibleDescription(
		ui->trackingStatusLabel->text() + QStringLiteral(". ") +
		ui->volumeHintLabel->text());
	ui->curvePreview->setAccessibleDescription(
		ui->curveMetaLabel->text() + QStringLiteral(". ") +
		ui->profileSummaryLabel->text());

	ui->curvePreview->update();
}

void LoudnessCorrectionStudioDialog::paintCurvePreview(QPainter& painter) const
{
	painter.setRenderHint(QPainter::Antialiasing, true);
	const bool highContrast = isHighContrastEnabled();
	const QPalette applicationPalette = QApplication::palette();
	QColor grid = applicationPalette.color(QPalette::Mid);
	QColor baseline = applicationPalette.color(QPalette::WindowText);
	QColor accent = applicationPalette.color(QPalette::Highlight);
	QColor accentText = applicationPalette.color(QPalette::HighlightedText);
	QColor label = applicationPalette.color(QPalette::Text);
	grid.setAlpha(highContrast ? 255 : 105);
	baseline.setAlpha(highContrast ? 255 : 145);
	accent.setAlpha(255);
	accentText.setAlpha(255);
	label.setAlpha(highContrast ? 255 : 190);

	QRectF plot = QRectF(ui->curvePreview->rect()).adjusted(14.0, 8.0, -14.0, -24.0);
	if (plot.width() <= 1.0 || plot.height() <= 1.0)
		return;
	if (!std::isfinite(listeningVolumeDb()))
	{
		painter.setPen(label);
		painter.drawText(plot, Qt::AlignCenter, tr("Check after apply"));
		return;
	}

	const double referenceLevel = ui->refLevelSpinBox->value();
	const double currentLevel = (std::max)(0.0, (std::min)(100.0,
		referenceLevel + listeningVolumeDb() - ui->refOffsetSpinBox->value()));
	const double strength = ui->strengthSpinBox->value();
	double gains[LoudnessProfile::FREQUENCY_COUNT];
	double maximumMagnitude = 0.0;
	for (size_t index = 0; index < LoudnessProfile::FREQUENCY_COUNT; ++index)
	{
		gains[index] = strength * LoudnessProfile::computeContourDelta(
			currentLevel, referenceLevel, index);
		maximumMagnitude = (std::max)(maximumMagnitude, std::abs(gains[index]));
	}
	const double range = (std::min)(48.0,
		(std::max)(12.0, std::ceil(maximumMagnitude / 6.0) * 6.0));
	const double centerY = plot.center().y();
	const double minLog = std::log10(20.0);
	const double maxLog = std::log10(12500.0);

	auto mapX = [&](double frequency) {
		return plot.left() + (std::log10(frequency) - minLog) /
			(maxLog - minLog) * plot.width();
	};
	auto mapY = [&](double gain) {
		const double limited = (std::max)(-range, (std::min)(range, gain));
		return centerY - limited / range * plot.height() * 0.46;
	};

	painter.setPen(QPen(grid, 1.0));
	for (int line = -2; line <= 2; ++line)
	{
		const double y = centerY + line * plot.height() * 0.2;
		painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
	}
	const double guideFrequencies[] = { 20.0, 100.0, 1000.0, 10000.0, 12500.0 };
	for (double frequency : guideFrequencies)
	{
		const double x = mapX(frequency);
		painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
	}

	painter.setPen(QPen(baseline, 1.2));
	painter.drawLine(QPointF(plot.left(), centerY), QPointF(plot.right(), centerY));

	QPainterPath curve;
	for (size_t index = 0; index < LoudnessProfile::FREQUENCY_COUNT; ++index)
	{
		const QPointF point(mapX(LoudnessProfile::LOUDNESS_PROFILE_TABLE[index].frequency),
			mapY(gains[index]));
		if (index == 0)
			curve.moveTo(point);
		else
			curve.lineTo(point);
	}

	QPainterPath fill(curve);
	fill.lineTo(plot.right(), centerY);
	fill.lineTo(plot.left(), centerY);
	fill.closeSubpath();
	if (!highContrast)
	{
		QColor fillStart = accent;
		QColor fillEnd = accent;
		fillStart.setAlpha(74);
		fillEnd.setAlpha(8);
		QLinearGradient areaGradient(0.0, plot.top(), 0.0, plot.bottom());
		areaGradient.setColorAt(0.0, fillStart);
		areaGradient.setColorAt(1.0, fillEnd);
		painter.fillPath(fill, areaGradient);

		QColor glow = accent;
		glow.setAlpha(48);
		painter.setPen(QPen(glow, 7.0,
			Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter.drawPath(curve);
	}
	painter.setPen(QPen(accent, highContrast ? 3.0 : 2.3,
		Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.drawPath(curve);

	const size_t referenceIndex = LoudnessProfile::REFERENCE_FREQUENCY_INDEX;
	painter.setBrush(accent);
	painter.setPen(QPen(accentText, highContrast ? 2.5 : 2.0));
	painter.drawEllipse(QPointF(mapX(1000.0), mapY(gains[referenceIndex])), 4.5, 4.5);

	QFont axisFont = painter.font();
	axisFont.setPointSizeF(8.0);
	painter.setFont(axisFont);
	painter.setPen(label);
	painter.drawText(QRectF(plot.left(), plot.bottom() + 5.0, 70.0, 16.0),
		Qt::AlignLeft | Qt::AlignVCenter, tr("20 Hz"));
	painter.drawText(QRectF(mapX(1000.0) - 35.0, plot.bottom() + 5.0, 70.0, 16.0),
		Qt::AlignHCenter | Qt::AlignVCenter, tr("1 kHz"));
	painter.drawText(QRectF(plot.right() - 90.0, plot.bottom() + 5.0, 90.0, 16.0),
		Qt::AlignRight | Qt::AlignVCenter, tr("12.5 kHz"));
	painter.drawText(QRectF(plot.left() + 4.0, plot.top() + 2.0, 80.0, 16.0),
		Qt::AlignLeft | Qt::AlignVCenter, tr("±%1 dB").arg(QString::number(range, 'f', 0)));
}

void LoudnessCorrectionStudioDialog::refreshStatusStyle(const char* status)
{
	ui->trackingStatusLabel->setProperty("status", status);
	ui->trackingStatusLabel->style()->unpolish(ui->trackingStatusLabel);
	ui->trackingStatusLabel->style()->polish(ui->trackingStatusLabel);
	ui->trackingStatusLabel->update();
}
