/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026  Equalizer APO contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "ModernTheme.h"
#include "StudioMotion.h"

#include "Editor/helpers/GUIHelper.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLibrary>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <algorithm>
#include <cmath>

namespace
{
	QString requestedThemeOverride()
	{
		return qEnvironmentVariable("EQAPO_UI_THEME").trimmed().toLower();
	}

	struct ThemeColors
	{
		QString canvas;
		QString surface;
		QString raised;
		QString border;
		QString borderStrong;
		QString text;
		QString muted;
		QString disabled;
		QString accent;
		QString accentHover;
		QString accentSoft;
		QString accentText;
		QString warning;
		QString danger;
		QString shadow;
	};

	double linearColorChannel(double channel)
	{
		channel /= 255.0;
		return channel <= 0.04045
			? channel / 12.92
			: std::pow((channel + 0.055) / 1.055, 2.4);
	}

	double relativeLuminance(const QColor& color)
	{
		return 0.2126 * linearColorChannel(color.red())
			+ 0.7152 * linearColorChannel(color.green())
			+ 0.0722 * linearColorChannel(color.blue());
	}

	double contrastRatio(const QColor& first, const QColor& second)
	{
		const double lighter = (std::max)(relativeLuminance(first), relativeLuminance(second));
		const double darker = (std::min)(relativeLuminance(first), relativeLuminance(second));
		return (lighter + 0.05) / (darker + 0.05);
	}

	QColor blend(const QColor& from, const QColor& to, double amount)
	{
		amount = std::clamp(amount, 0.0, 1.0);
		return QColor(
			qRound(from.red() + (to.red() - from.red()) * amount),
			qRound(from.green() + (to.green() - from.green()) * amount),
			qRound(from.blue() + (to.blue() - from.blue()) * amount));
	}

	QColor ensureContrast(QColor color, const QColor& background, bool dark)
	{
		const QColor target = dark ? QColor(Qt::white) : QColor(Qt::black);
		for (double amount = 0.0; contrastRatio(color, background) < 4.5 && amount < 1.0; amount += 0.04)
			color = blend(color, target, 0.04);
		return color;
	}

	QColor contrastingText(const QColor& background)
	{
		const QColor darkText(QStringLiteral("#111111"));
		const QColor lightText(QStringLiteral("#FFFFFF"));
		return contrastRatio(darkText, background) >= contrastRatio(lightText, background)
			? darkText
			: lightText;
	}

	QColor systemAccentColor(const QApplication& application)
	{
#ifdef Q_OS_WIN
		Q_UNUSED(application);
		QLibrary dwmApi(QStringLiteral("dwmapi"));
		using DwmGetColorizationColorFunction = HRESULT (WINAPI *)(DWORD*, BOOL*);
		auto getColorizationColor = reinterpret_cast<DwmGetColorizationColorFunction>(
			dwmApi.resolve("DwmGetColorizationColor"));
		DWORD colorization = 0;
		BOOL opaqueBlend = FALSE;
		if (getColorizationColor && SUCCEEDED(getColorizationColor(&colorization, &opaqueBlend)))
		{
			Q_UNUSED(opaqueBlend);
			return QColor(
				static_cast<int>((colorization >> 16) & 0xff),
				static_cast<int>((colorization >> 8) & 0xff),
				static_cast<int>(colorization & 0xff));
		}

		const COLORREF highlight = GetSysColor(COLOR_HIGHLIGHT);
		return QColor(GetRValue(highlight), GetGValue(highlight), GetBValue(highlight));
#else
		return application.palette().color(QPalette::Highlight);
#endif
	}

	bool highContrastEnabled(const QApplication& application)
	{
		const QString override = requestedThemeOverride();
		if (override == QStringLiteral("high-contrast")
			|| override == QStringLiteral("highcontrast"))
			return true;
		if (override == QStringLiteral("light") || override == QStringLiteral("dark"))
			return false;

		Q_UNUSED(application);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
		if (const QAccessibilityHints* accessibility = QGuiApplication::styleHints()->accessibility())
		{
			if (accessibility->contrastPreference() == Qt::ContrastPreference::HighContrast)
				return true;
		}
#endif
#ifdef Q_OS_WIN
		HIGHCONTRASTW highContrast = {};
		highContrast.cbSize = sizeof(highContrast);
		if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0))
			return (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
#endif
		return false;
	}

	bool darkModeForApplication(QApplication& application)
	{
		const QString override = requestedThemeOverride();
		if (override == QStringLiteral("dark"))
			return true;
		if (override == QStringLiteral("light"))
			return false;

		const Qt::ColorScheme scheme = application.styleHints()->colorScheme();
		if (scheme == Qt::ColorScheme::Dark)
			return true;
		if (scheme == Qt::ColorScheme::Light)
			return false;

		const QVariant remembered = application.property("eqapoModernThemeDark");
		if (remembered.isValid())
			return remembered.toBool();
		return application.palette().color(QPalette::Window).lightnessF() < 0.5;
	}

	ThemeColors colorsForMode(bool dark, const QColor& requestedAccent)
	{
		const QColor canvas(dark ? QStringLiteral("#17191C") : QStringLiteral("#EFF1F4"));
		const QColor accent = ensureContrast(
			requestedAccent.isValid() ? requestedAccent : QColor(QStringLiteral("#0067C0")),
			canvas,
			dark);
		const QColor accentText = contrastingText(accent);
		const QColor accentHover = blend(accent, dark ? QColor(Qt::white) : QColor(Qt::black), 0.12);
		const QColor surface(dark ? QStringLiteral("#22252A") : QStringLiteral("#FFFFFF"));
		const QColor accentSoft = blend(surface, accent, dark ? 0.24 : 0.14);

		if (dark)
		{
			return {
				canvas.name(), surface.name(), "#2B2F35", "#363C44",
				"#626C78", "#F2F4F7", "#B7BFC9", "#A1AAB6",
				accent.name(), accentHover.name(), accentSoft.name(), accentText.name(),
				"#F2C879", "#FF8A86", "#111111"
			};
		}

		return {
			canvas.name(), surface.name(), "#E8ECF1", "#D3D9E2",
			"#97A2B2", "#202630", "#536071", "#6F6F6F",
			accent.name(), accentHover.name(), accentSoft.name(), accentText.name(),
			"#8A5A00", "#B3261E", "#B8B8B8"
		};
	}

	QPalette createPalette(const ThemeColors& colors)
	{
		QPalette palette;
		palette.setColor(QPalette::Window, QColor(colors.canvas));
		palette.setColor(QPalette::WindowText, QColor(colors.text));
		palette.setColor(QPalette::Base, QColor(colors.surface));
		palette.setColor(QPalette::AlternateBase, QColor(colors.raised));
		palette.setColor(QPalette::ToolTipBase, QColor(colors.raised));
		palette.setColor(QPalette::ToolTipText, QColor(colors.text));
		palette.setColor(QPalette::Text, QColor(colors.text));
		palette.setColor(QPalette::Button, QColor(colors.raised));
		palette.setColor(QPalette::ButtonText, QColor(colors.text));
		palette.setColor(QPalette::BrightText, QColor(colors.accentText));
		palette.setColor(QPalette::Light, QColor(colors.surface));
		palette.setColor(QPalette::Midlight, QColor(colors.raised));
		palette.setColor(QPalette::Mid, QColor(colors.border));
		palette.setColor(QPalette::Dark, QColor(colors.shadow));
		palette.setColor(QPalette::Shadow, QColor(colors.shadow));
		palette.setColor(QPalette::Highlight, QColor(colors.accent));
		palette.setColor(QPalette::HighlightedText, QColor(colors.accentText));
		palette.setColor(QPalette::Link, QColor(colors.accent));
		palette.setColor(QPalette::LinkVisited, QColor(colors.accentHover));
		palette.setColor(QPalette::PlaceholderText, QColor(colors.muted));
		palette.setColor(QPalette::Accent, QColor(colors.accent));

		palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(colors.disabled));
		palette.setColor(QPalette::Disabled, QPalette::Text, QColor(colors.disabled));
		palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(colors.disabled));
		palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(colors.border));
		palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(colors.disabled));
		return palette;
	}

	QPalette createHighContrastPalette(const QApplication& application)
	{
		const QString override = requestedThemeOverride();
		if (override == QStringLiteral("high-contrast")
			|| override == QStringLiteral("highcontrast"))
		{
			const QColor background(Qt::black);
			const QColor foreground(Qt::white);
			const QColor highlight(Qt::yellow);
			const QColor highlightedText(Qt::black);
			const QColor disabled(QStringLiteral("#A0A0A0"));
			QPalette palette;
			palette.setColor(QPalette::Window, background);
			palette.setColor(QPalette::WindowText, foreground);
			palette.setColor(QPalette::Base, background);
			palette.setColor(QPalette::AlternateBase, background);
			palette.setColor(QPalette::ToolTipBase, background);
			palette.setColor(QPalette::ToolTipText, foreground);
			palette.setColor(QPalette::Text, foreground);
			palette.setColor(QPalette::Button, background);
			palette.setColor(QPalette::ButtonText, foreground);
			palette.setColor(QPalette::BrightText, foreground);
			palette.setColor(QPalette::Light, foreground);
			palette.setColor(QPalette::Midlight, foreground);
			palette.setColor(QPalette::Mid, foreground);
			palette.setColor(QPalette::Dark, foreground);
			palette.setColor(QPalette::Shadow, foreground);
			palette.setColor(QPalette::Highlight, highlight);
			palette.setColor(QPalette::HighlightedText, highlightedText);
			palette.setColor(QPalette::Link, QColor(Qt::cyan));
			palette.setColor(QPalette::LinkVisited, QColor(Qt::cyan));
			palette.setColor(QPalette::PlaceholderText, disabled);
			palette.setColor(QPalette::Accent, highlight);
			palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
			palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
			palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
			palette.setColor(QPalette::Disabled, QPalette::Highlight, background);
			palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
			return palette;
		}

#ifdef Q_OS_WIN
		Q_UNUSED(application);
		auto systemColor = [](int index)
		{
			const COLORREF color = GetSysColor(index);
			return QColor(GetRValue(color), GetGValue(color), GetBValue(color));
		};

		const QColor window = systemColor(COLOR_WINDOW);
		const QColor windowText = systemColor(COLOR_WINDOWTEXT);
		const QColor button = systemColor(COLOR_BTNFACE);
		const QColor buttonText = systemColor(COLOR_BTNTEXT);
		const QColor highlight = systemColor(COLOR_HIGHLIGHT);
		const QColor highlightedText = systemColor(COLOR_HIGHLIGHTTEXT);
		const QColor disabled = systemColor(COLOR_GRAYTEXT);
		const QColor border = systemColor(COLOR_WINDOWFRAME);

		QPalette palette;
		palette.setColor(QPalette::Window, window);
		palette.setColor(QPalette::WindowText, windowText);
		palette.setColor(QPalette::Base, window);
		palette.setColor(QPalette::AlternateBase, button);
		palette.setColor(QPalette::ToolTipBase, systemColor(COLOR_INFOBK));
		palette.setColor(QPalette::ToolTipText, systemColor(COLOR_INFOTEXT));
		palette.setColor(QPalette::Text, windowText);
		palette.setColor(QPalette::Button, button);
		palette.setColor(QPalette::ButtonText, buttonText);
		palette.setColor(QPalette::BrightText, highlightedText);
		palette.setColor(QPalette::Light, systemColor(COLOR_BTNHIGHLIGHT));
		palette.setColor(QPalette::Midlight, button);
		palette.setColor(QPalette::Mid, border);
		palette.setColor(QPalette::Dark, border);
		palette.setColor(QPalette::Shadow, border);
		palette.setColor(QPalette::Highlight, highlight);
		palette.setColor(QPalette::HighlightedText, highlightedText);
		palette.setColor(QPalette::Link, systemColor(COLOR_HOTLIGHT));
		palette.setColor(QPalette::LinkVisited, systemColor(COLOR_HOTLIGHT));
		palette.setColor(QPalette::PlaceholderText, disabled);
		palette.setColor(QPalette::Accent, highlight);
		palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
		palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
		palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
		palette.setColor(QPalette::Disabled, QPalette::Highlight, button);
		palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
		return palette;
#else
		return application.style()->standardPalette();
#endif
	}

	void replaceGeometryTokens(QString& style)
	{
		// Qt 6 uses device-independent coordinates. Retain compatibility with
		// explicitly opted-out legacy launches without double-scaling normal
		// per-monitor-DPI-aware sessions.
		const bool usesManualScaling = qEnvironmentVariable("QT_ENABLE_HIGHDPI_SCALING") == QStringLiteral("0");
		const auto themePixel = [usesManualScaling](int value) {
			return usesManualScaling ? GUIHelper::scale(value) : value;
		};

		const int fontHeight = QFontMetrics(QApplication::font()).height();
		const int controlHeight = (std::max)(26, fontHeight + 10);
		const int compactButtonSize = (std::max)(20, fontHeight + 4);
		style.replace("@controlHeight", QString::number(themePixel(controlHeight)));
		style.replace("@controlOuterHeight", QString::number(themePixel(controlHeight) + 2));
		style.replace("@dialogButtonWidth", QString::number(themePixel(88)));
		style.replace("@compactButtonSize", QString::number(themePixel(compactButtonSize)));
		style.replace("@indicatorSize", QString::number(themePixel(16)));
		style.replace("@radioRadius", QString::number(themePixel(8)));
		style.replace("@spinButtonWidth", QString::number(themePixel(16)));
	}

	QString createHighContrastStyleSheet();

	QString createStyleSheet(const ThemeColors& colors)
	{
		QString style = QStringLiteral(R"QSS(
QMainWindow#MainWindow, QDialog {
	background-color: @canvas;
	color: @text;
}

QFrame#studioHeader {
    background: @canvas;
    border: none;
    border-bottom: 1px solid @border;
}

QFrame#studioHeader QComboBox[compactAudioControl="true"] {
	min-height: 24px;
	padding-top: 2px;
	padding-bottom: 2px;
}

QFrame#referenceContourCard, QFrame#offsetContourCard, QFrame#strengthContourCard,
QFrame#listeningCard {
    background: @surface;
    border: 1px solid @border;
    border-radius: 6px;
}
QFrame#listeningCard QLabel#volumeSourceLabel {
    color: @muted;
}
QLabel#studioWordmark {
    color: @text;
    font-size: 22pt;
    font-weight: 700;
    letter-spacing: 3px;
}
QLabel#studioEdition {
    color: @accent;
    font-size: 8pt;
    font-weight: 600;
    letter-spacing: 2px;
}
QLabel#studioSection {
    color: @muted;
    font-weight: 600;
    padding-left: 16px;
    border-left: 1px solid @borderStrong;
}
QFrame#studioEmptyState {
    background: @canvas;
    border: none;
}
QLabel#studioEmptyTitle {
    color: @text;
    font-size: 18pt;
    font-weight: 600;
}
QLabel#studioEmptyHint {
    color: @muted;
}
QLabel#analysisStateLabel {
    color: @muted;
    padding: 5px 0;
}
QLabel#headroomValueLabel {
    font-family: "Cascadia Mono";
    font-weight: 700;
    color: @accent;
}
QWidget#centralWidget {
	background-color: @canvas;
}

QMenuBar {
	background-color: @surface;
	color: @text;
	border: none;
	border-bottom: 1px solid @border;
	padding: 2px 6px;
}

QMenuBar::item {
	background: transparent;
	padding: 6px 10px;
	margin: 1px 2px;
}

QMenuBar::item:selected, QMenuBar::item:pressed {
	background-color: @raised;
	color: @accent;
}

QMenu {
	border-radius: 10px;
	background-color: @surface;
	color: @text;
	border: 1px solid @border;
	padding: 6px;
}

QMenu::item {
	border-radius: 5px;
	padding: 6px 28px 6px 10px;
	margin: 1px;
}

QMenu::item:selected {
	background-color: @accentSoft;
	color: @text;
}

QMenu::item:disabled {
	color: @disabled;
}

QMenu::separator {
	height: 1px;
	background-color: @border;
	margin: 5px 8px;
}

QToolBar#mainToolBar {
	background-color: @surface;
	border: none;
	border-bottom: 1px solid @border;
	spacing: 4px;
	padding: 7px 10px;
}

QToolBar#mainToolBar::separator {
	background-color: @border;
	width: 1px;
	margin: 5px 7px;
}

QToolBar#mainToolBar QToolButton {
	border-radius: 6px;
	background-color: transparent;
	color: @text;
	border: 1px solid transparent;
	min-width: @controlHeightpx;
	min-height: @controlHeightpx;
	padding: 0;
}

QToolBar#mainToolBar QToolButton:hover {
	background-color: @raised;
	border-color: @border;
}

QToolBar#mainToolBar QToolButton:focus {
	border-color: @accent;
}

QToolBar#mainToolBar QToolButton:pressed,
QToolBar#mainToolBar QToolButton:checked {
	background-color: @accentSoft;
	border-color: @accent;
}

QToolBar#mainToolBar QLabel#workspaceBrand {
	border-radius: 6px;
	background-color: @accent;
	color: @accentText;
	border: 1px solid transparent;
	font-weight: 700;
	min-height: @controlHeightpx;
	padding: 0 9px;
}

QToolBar#mainToolBar QLabel[toolbarRole="context"] {
	color: @muted;
	font-weight: 600;
	min-height: @controlOuterHeightpx;
}

QToolBar#mainToolBar QCheckBox {
	color: @text;
	spacing: 6px;
	min-height: @controlOuterHeightpx;
}

QToolBar#mainToolBar QComboBox {
	background-color: @raised;
	border-color: @borderStrong;
}

QToolBar#workspaceToolBar {
	background-color: @surface;
	border: none;
	border-bottom: 1px solid @border;
	spacing: 5px;
	padding: 5px 10px;
}

QToolBar#workspaceToolBar::separator {
	background-color: @border;
	width: 1px;
	margin: 5px 7px;
}

QToolBar#workspaceToolBar QToolButton {
	border-radius: 6px;
	background-color: transparent;
	color: @text;
	border: 1px solid transparent;
	min-width: @controlHeightpx;
	min-height: @controlHeightpx;
	padding: 0 7px;
}

QToolBar#workspaceToolBar QToolButton:hover,
QToolBar#workspaceToolBar QToolButton:focus {
	background-color: @raised;
	border-color: @accent;
}

QToolBar#workspaceToolBar QToolButton:pressed,
QToolBar#workspaceToolBar QToolButton:checked {
	background-color: @accentSoft;
	border-color: @accent;
}

QToolBar#workspaceToolBar QLabel[toolbarRole="context"] {
	color: @muted;
	font-weight: 600;
}

QToolBar#workspaceToolBar QLabel[workspaceStatus="true"] {
	color: @muted;
	padding: 0 6px;
}

QToolBar#workspaceToolBar QLabel[workspaceStatus="true"][statusLevel="warning"] {
	color: @warning;
}

QToolBar#workspaceToolBar QLabel[workspaceStatus="true"][statusLevel="danger"] {
	color: @danger;
}

QTabWidget#tabWidget::pane {
	background-color: @canvas;
	border: 1px solid @border;
	top: -1px;
}

QTabBar::tab {
	border-top-left-radius: 7px;
	border-top-right-radius: 7px;
	background-color: transparent;
	color: @muted;
	border: none;
	border-bottom: 2px solid transparent;
	padding: 8px 14px;
	margin-right: 2px;
}

QTabBar::tab:hover {
	background-color: @raised;
	color: @text;
}

QTabBar::tab:selected {
	background-color: @surface;
	color: @text;
	border-bottom-color: @accent;
}

QDockWidget#analysisDockWidget {
	color: @text;
}

QDockWidget#analysisDockWidget::title {
	background-color: @surface;
	border: 1px solid @border;
	padding: 7px;
}

QDockWidget#analysisDockWidget QWidget#dockWidgetContents {
	background-color: @canvas;
}

QDockWidget#analysisDockWidget QGroupBox {
	border-radius: 8px;
	background-color: @surface;
	border: 1px solid @border;
	margin-top: 11px;
}

QDockWidget#analysisDockWidget QGroupBox::title {
	subcontrol-origin: margin;
	left: 9px;
	padding: 0 5px;
	color: @muted;
	font-weight: 600;
}

QDockWidget#analysisDockWidget QLabel#peakGainValueLabel,
QDockWidget#analysisDockWidget QLabel#latencyValueLabel,
QDockWidget#analysisDockWidget QLabel#initTimeValueLabel,
QDockWidget#analysisDockWidget QLabel#cpuUsageValueLabel {
	font-family: "Cascadia Mono";
	font-weight: 600;
}

QLabel[statusLevel="warning"] {
	color: @warning;
}

QLabel[statusLevel="danger"] {
	color: @danger;
}

QPlainTextEdit[statusLevel="warning"] {
	border-color: @warning;
}

QDockWidget#analysisDockWidget QGraphicsView#graphicsView {
	background-color: @surface;
	border: 1px solid @border;
}

QToolButton#resetAnalysisViewButton {
	background-color: @raised;
	color: @text;
	border: 1px solid @borderStrong;
	min-height: @controlHeightpx;
	padding: 0 9px;
}

QToolButton#resetAnalysisViewButton:hover {
	color: @accent;
	border-color: @accent;
}

QToolButton#resetAnalysisViewButton:focus {
	border-color: @accent;
}

QToolButton#resetAnalysisViewButton:pressed {
	background-color: @accentSoft;
	border-color: @accent;
}

QToolButton#resetAnalysisViewButton:disabled {
	background-color: @canvas;
	color: @disabled;
	border-color: @border;
}

QScrollArea {
	background-color: @canvas;
	border: none;
}

FilterTable, FilterTableRow, FilterTableRow QStackedWidget {
	background: transparent;
	border: none;
}

FilterTableRow QLabel#labelNumber {
	color: @accent;
	font-weight: 600;
}

DeviceFilterGUI QTreeWidget[deviceMatchState="missing"] {
	color: @danger;
	border-color: @danger;
}

DeviceFilterGUI QTreeWidget[deviceMatchState="missing"]:disabled {
	color: @disabled;
	border-color: @border;
}

LoudnessCorrectionFilterGUI QLabel#label {
	font-weight: 600;
}

LoudnessCorrectionFilterGUI QLabel#label_2 {
	color: @muted;
}

FilterTableRow QToolBar {
	background: transparent;
	border: none;
}

FilterTableRow QToolButton {
	border-radius: 4px;
	background: transparent;
	border: 1px solid transparent;
	min-width: @compactButtonSizepx;
	min-height: @compactButtonSizepx;
	padding: 1px;
}

QToolBar#filterAddBar QToolButton {
	background: transparent;
	border: 1px solid transparent;
	min-height: @compactButtonSizepx;
	padding: 1px 8px;
}

FilterTableRow QToolButton:hover,
QToolBar#filterAddBar QToolButton:hover {
	background-color: @accentSoft;
	border-color: @border;
}

FilterTableRow QToolButton:focus,
FilterTableRow QToolButton:pressed,
QToolBar#filterAddBar QToolButton:focus,
QToolBar#filterAddBar QToolButton:pressed {
	background-color: @accentSoft;
	border-color: @accent;
}

QPushButton {
	border-radius: 6px;
	background-color: @raised;
	color: @text;
	border: 1px solid @borderStrong;
	min-height: @controlHeightpx;
	padding: 0 12px;
}

QDialogButtonBox QPushButton {
	min-width: @dialogButtonWidthpx;
}

QPushButton:hover {
	border-color: @accent;
	color: @accent;
}

QPushButton:focus {
	border-color: @accent;
}

QPushButton:pressed {
	background-color: @accentSoft;
}

QPushButton:default {
	background-color: @accent;
	border-color: @accent;
	color: @accentText;
	font-weight: 600;
}

QPushButton:disabled {
	background-color: @canvas;
	border-color: @border;
	color: @disabled;
}

QLineEdit, QComboBox, QAbstractSpinBox {
	border-radius: 5px;
	background-color: @raised;
	color: @text;
	border: 1px solid @border;
	min-height: @controlHeightpx;
	padding: 0 7px;
	selection-background-color: @accent;
	selection-color: @accentText;
}

QPlainTextEdit, QTextEdit {
	background-color: @raised;
	color: @text;
	border: 1px solid @border;
	padding: 5px 7px;
	selection-background-color: @accent;
	selection-color: @accentText;
}

QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover,
QPlainTextEdit:hover, QTextEdit:hover {
	border-color: @borderStrong;
}

QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus,
QPlainTextEdit:focus, QTextEdit:focus {
	border-color: @accent;
}

QLineEdit:disabled, QComboBox:disabled, QAbstractSpinBox:disabled,
QPlainTextEdit:disabled, QTextEdit:disabled {
	background-color: @canvas;
	color: @disabled;
}

QComboBox::drop-down {
	border: none;
	width: 22px;
}

QCheckBox, QRadioButton {
	color: @text;
	spacing: 6px;
}

QCheckBox:disabled, QRadioButton:disabled {
	color: @disabled;
}

QCheckBox::indicator, QRadioButton::indicator, QGroupBox::indicator {
	width: @indicatorSizepx;
	height: @indicatorSizepx;
	background-color: @raised;
	border: 1px solid @borderStrong;
	border-radius: 0px;
}

QRadioButton::indicator {
	border-radius: @radioRadiuspx;
}

QCheckBox::indicator:hover, QRadioButton::indicator:hover,
QGroupBox::indicator:hover {
	border-color: @accent;
}

QCheckBox::indicator:focus, QRadioButton::indicator:focus,
QGroupBox::indicator:focus {
	border-color: @accent;
}

QCheckBox::indicator:checked, QRadioButton::indicator:checked,
QGroupBox::indicator:checked {
	background-color: @accent;
	border-color: @accent;
}

QCheckBox::indicator:checked, QGroupBox::indicator:checked {
	image: url(@checkIcon);
}

QRadioButton::indicator:checked {
	image: url(@radioIcon);
}

QCheckBox::indicator:disabled, QRadioButton::indicator:disabled,
QGroupBox::indicator:disabled {
	background-color: @canvas;
	border-color: @border;
}

QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
	background: transparent;
	border: none;
	border-left: 1px solid @border;
	width: @spinButtonWidthpx;
}

QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover {
	background-color: @accentSoft;
}

QScrollArea#filterTable QGroupBox {
	border: 1px solid @border;
	border-radius: 6px;
	margin-top: 8px;
	padding-top: 4px;
	padding-bottom: 2px;
}

QScrollArea#filterTable QGroupBox::title {
	subcontrol-origin: margin;
	left: 8px;
	padding: 0 4px;
	color: @muted;
	font-weight: 600;
}

QDialog QGroupBox {
	border: 1px solid @border;
	margin-top: 10px;
}

QDialog QGroupBox::title {
	subcontrol-origin: margin;
	left: 8px;
	padding: 0 4px;
	color: @muted;
	font-weight: 600;
}

QDialog#LoudnessCorrectionFilterGUIDialog QLabel#titleLabel {
	font-size: 18pt;
	font-weight: 650;
}

QDialog#LoudnessCorrectionFilterGUIDialog QLabel#subtitleLabel,
QDialog#LoudnessCorrectionFilterGUIDialog QLabel#footerHintLabel {
	color: @muted;
}

QDialog#LoudnessCorrectionFilterGUIDialog QLabel#safetyStatusLabel {
	background-color: @surface;
	border: 1px solid @warning;
	padding: 8px 10px;
	font-weight: 600;
}

QDialog#LoudnessCorrectionFilterGUIDialog QLabel#playbackStatusLabel {
	padding: 0 6px;
	font-weight: 600;
}

QTreeView, QTableView, QListView {
	background-color: @surface;
	alternate-background-color: @raised;
	color: @text;
	border: 1px solid @border;
	selection-background-color: @accentSoft;
	selection-color: @text;
}

QTreeView::item:hover, QTableView::item:hover, QListView::item:hover {
	background-color: @raised;
}

QTreeView::item:selected, QTableView::item:selected, QListView::item:selected {
	background-color: @accentSoft;
	color: @text;
}

QHeaderView::section {
	background-color: @raised;
	color: @muted;
	border: none;
	border-right: 1px solid @border;
	border-bottom: 1px solid @border;
	font-weight: 600;
}

QScrollBar:vertical {
	background: @canvas;
	width: 11px;
	margin: 2px;
}

QScrollBar:horizontal {
	background: @canvas;
	height: 11px;
	margin: 2px;
}

QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
	border-radius: 3px;
	background: @borderStrong;
	min-height: 24px;
	min-width: 24px;
}

QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {
	background: @accent;
}

QScrollBar::add-line, QScrollBar::sub-line,
QScrollBar::add-page, QScrollBar::sub-page {
	background: none;
	border: none;
}

QSplitter::handle {
	background-color: @border;
}

QSplitter::handle:hover {
	background-color: @accent;
}

QProgressBar {
	background-color: @raised;
	border: 1px solid @border;
	text-align: center;
}

QProgressBar::chunk {
	background-color: @accent;
}

QToolTip {
	background-color: @raised;
	color: @text;
	border: 1px solid @borderStrong;
	padding: 5px;
}
)QSS");

		style.replace("@canvas", colors.canvas);
		style.replace("@surface", colors.surface);
		style.replace("@raised", colors.raised);
		style.replace("@borderStrong", colors.borderStrong);
		style.replace("@border", colors.border);
		style.replace("@text", colors.text);
		style.replace("@muted", colors.muted);
		style.replace("@disabled", colors.disabled);
		style.replace("@warning", colors.warning);
		style.replace("@danger", colors.danger);
		style.replace("@accentHover", colors.accentHover);
		style.replace("@accentSoft", colors.accentSoft);
		style.replace("@accentText", colors.accentText);
		style.replace("@accent", colors.accent);
		const bool useDarkGlyph = QColor(colors.accentText).lightnessF() < 0.5;
		style.replace(
			"@checkIcon",
			useDarkGlyph ? ":/icons/modern/check-dark.svg" : ":/icons/modern/check.svg");
		style.replace(
			"@radioIcon",
			useDarkGlyph ? ":/icons/modern/radio-dot-dark.svg" : ":/icons/modern/radio-dot.svg");
		replaceGeometryTokens(style);
		return style;
	}

	QString createHighContrastStyleSheet()
	{
		const QPalette palette = QApplication::palette();
		const auto activeColor = [&palette](QPalette::ColorRole role)
		{
			return palette.color(QPalette::Active, role).name(QColor::HexRgb);
		};
		const auto disabledColor = [&palette](QPalette::ColorRole role)
		{
			return palette.color(QPalette::Disabled, role).name(QColor::HexRgb);
		};

		// Applying even a geometry-only stylesheet makes Qt stop drawing parts of
		// the native Windows controls. Build the complete studio theme
		// from the active system high-contrast palette so button frames, group
		// boundaries and check indicators remain visible at every DPI.
		const ThemeColors colors = {
			activeColor(QPalette::Window),
			activeColor(QPalette::Base),
			activeColor(QPalette::Button),
			activeColor(QPalette::WindowText),
			activeColor(QPalette::ButtonText),
			activeColor(QPalette::WindowText),
			activeColor(QPalette::WindowText),
			disabledColor(QPalette::Text),
			activeColor(QPalette::Highlight),
			activeColor(QPalette::Highlight),
			activeColor(QPalette::Button),
			activeColor(QPalette::HighlightedText),
			activeColor(QPalette::WindowText),
			activeColor(QPalette::WindowText),
			activeColor(QPalette::WindowText)
		};

		QString style = createStyleSheet(colors);
		style += QStringLiteral(R"QSS(
QToolBar#mainToolBar QToolButton,
QToolBar#workspaceToolBar QToolButton,
FilterTableRow QToolButton,
QToolBar#filterAddBar QToolButton,
QToolButton#resetAnalysisViewButton {
	background-color: palette(button);
	border: 1px solid palette(window-text);
	color: palette(button-text);
}

QMenu::item:selected,
QToolBar#mainToolBar QToolButton:checked,
QToolBar#workspaceToolBar QToolButton:checked,
QTreeView::item:selected,
QTableView::item:selected,
QListView::item:selected {
	background-color: palette(highlight);
	color: palette(highlighted-text);
}
)QSS");
		return style;
	}

	class ThemeMonitor final : public QObject, public QAbstractNativeEventFilter
	{
	public:
		explicit ThemeMonitor(QApplication& application)
			: QObject(&application), m_application(&application)
		{
			application.installEventFilter(this);
			application.installNativeEventFilter(this);
			QObject::connect(
				application.styleHints(),
				&QStyleHints::colorSchemeChanged,
				this,
				[this](Qt::ColorScheme)
				{
					scheduleApply();
				});
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
			if (const QAccessibilityHints* accessibility = application.styleHints()->accessibility())
			{
				QObject::connect(
					accessibility,
					&QAccessibilityHints::contrastPreferenceChanged,
					this,
					[this](Qt::ContrastPreference)
					{
						scheduleApply();
					});
			}
#endif
		}

		~ThemeMonitor() override
		{
			if (QCoreApplication::instance() == m_application)
				m_application->removeNativeEventFilter(this);
		}

		bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override
		{
			Q_UNUSED(eventType);
			Q_UNUSED(result);
#ifdef Q_OS_WIN
			const MSG* nativeMessage = static_cast<const MSG*>(message);
			if (nativeMessage)
			{
				constexpr UINT DwmColorizationColorChanged = 0x0320;
				switch (nativeMessage->message)
				{
				case WM_THEMECHANGED:
				case WM_SYSCOLORCHANGE:
				case WM_SETTINGCHANGE:
				case DwmColorizationColorChanged:
					scheduleApply();
					break;
				default:
					break;
				}
			}
#else
			Q_UNUSED(message);
#endif
			return false;
		}

	protected:
		bool eventFilter(QObject*, QEvent* event) override
		{
			if (event->type() == QEvent::ThemeChange)
				scheduleApply();
			return false;
		}

	private:
		void scheduleApply()
		{
			if (m_applyScheduled)
				return;
			m_applyScheduled = true;
			QTimer::singleShot(75, this, [this]()
			{
				m_applyScheduled = false;
				if (m_application)
					ModernTheme::apply(*m_application);
			});
		}

		QApplication* m_application;
		bool m_applyScheduled = false;
	};
}

void ModernTheme::install(QApplication& application)
{
	if (application.property("eqapoModernThemeInstalled").toBool())
	{
		apply(application);
		return;
	}

	application.setProperty("eqapoModernThemeInstalled", true);
	new ThemeMonitor(application);
	StudioMotion::install(application);
	apply(application);
}

void ModernTheme::apply(QApplication& application)
{
	QFont applicationFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
	bool scaleIsValid = false;
	const qreal requestedFontScale = qEnvironmentVariable("EQAPO_UI_FONT_SCALE").toDouble(&scaleIsValid);
	if (scaleIsValid && requestedFontScale >= 0.75 && requestedFontScale <= 3.0)
	{
		if (applicationFont.pointSizeF() > 0.0)
			applicationFont.setPointSizeF(applicationFont.pointSizeF() * requestedFontScale);
		else if (applicationFont.pixelSize() > 0)
			applicationFont.setPixelSize(qRound(applicationFont.pixelSize() * requestedFontScale));
	}
	application.setFont(applicationFont);

	const bool highContrast = highContrastEnabled(application);
	application.setProperty("eqapoModernThemeHighContrast", highContrast);
	if (highContrast)
	{
		application.setPalette(createHighContrastPalette(application));
		application.setStyleSheet(createHighContrastStyleSheet());
		return;
	}

	const bool dark = darkModeForApplication(application);
	application.setProperty("eqapoModernThemeDark", dark);
	const ThemeColors colors = colorsForMode(dark, systemAccentColor(application));
	application.setPalette(createPalette(colors));
	application.setStyleSheet(createStyleSheet(colors));
}
