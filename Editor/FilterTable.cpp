/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2014  Jonas Thedering

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

#include <QDrag>
#include <QMimeData>
#include <QApplication>
#include <QSignalBlocker>
#include <QClipboard>
#include <QCursor>
#include <QLabel>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QToolButton>
#include <QScrollBar>
#include <QToolBar>
#include <QComboBox>
#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QDial>
#include <QIcon>
#include <QPainter>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QUuid>

#include <cmath>
#include <memory>

#include "MainWindow.h"
#include "helpers/VSTParameterParser.h"

namespace
{
const QSet<QString>& preampScopeBoundaries()
{
	static const QSet<QString> boundaries = {
		QStringLiteral("channel"), QStringLiteral("device"),
		QStringLiteral("if"), QStringLiteral("elseif"),
		QStringLiteral("else"), QStringLiteral("endif"),
		QStringLiteral("include"), QStringLiteral("stage")
	};
	return boundaries;
}

bool isPreampScopeBoundary(const QString& line)
{
	const QString trimmed = line.trimmed();
	if (trimmed.isEmpty() || trimmed.startsWith('#'))
		return false;
	const int separator = trimmed.indexOf(':');
	return separator >= 0 && preampScopeBoundaries().contains(
		trimmed.left(separator).trimmed().toCaseFolded());
}

bool parseEditablePreampLine(
	const QString& line,
	QString* prefix,
	double* gain,
	QString* suffix,
	bool* commaDecimal = nullptr)
{
	// The runtime accepts every C %lf spelling, including hexadecimal values,
	// and stops checking after the conversion succeeds.  Auto preamp supports a
	// deliberately smaller, unambiguous decimal grammar so rewriting a numeric
	// prefix can never change the value's token boundary (for example -0x1p2).
	static const QRegularExpression pattern(QStringLiteral(
		R"(^(\s*Preamp\s*:\s*)([-+]?(?:(?:\d+(?:[.,]\d*)?)|(?:[.,]\d+))(?:[eE][-+]?\d+)?)(\s*(?:dB)?\s*(?:#.*)?)$)"));
	if (line.contains('`'))
		return false;
	const QRegularExpressionMatch match = pattern.match(line);
	if (!match.hasMatch())
		return false;

	bool validGain = false;
	const QString valueText = match.captured(2);
	const double parsedGain = QString(valueText).replace(',', '.').toDouble(
		&validGain);
	if (!validGain || !std::isfinite(parsedGain))
		return false;

	if (prefix != nullptr)
		*prefix = match.captured(1);
	if (gain != nullptr)
		*gain = parsedGain;
	if (suffix != nullptr)
		*suffix = match.captured(3);
	if (commaDecimal != nullptr)
		*commaDecimal = valueText.contains(',');
	return true;
}

struct FilterParameterToken
{
	int start = 0;
	int end = 0;
	QString value;
};

QList<FilterParameterToken> tokenizeFilterParameters(const QString& parameters)
{
	QList<FilterParameterToken> tokens;
	int index = 0;
	while (index < parameters.size())
	{
		while (index < parameters.size() && parameters[index] == ' ')
			++index;
		if (index >= parameters.size())
			break;

		FilterParameterToken token;
		token.start = index;
		bool inQuotes = false;
		for (; index < parameters.size(); ++index)
		{
			const QChar ch = parameters[index];
			if (ch == ' ' && !inQuotes)
				break;
			if (ch == '"')
			{
				inQuotes = !inQuotes;
				if (inQuotes && index > token.start && parameters[index - 1] == '"')
					token.value += '"';
			}
			else
			{
				token.value += ch;
			}
		}
		token.end = index;
		// Keep syntactically present empty quoted tokens. The production parser
		// drops their value, but the cloner still has to remove the original raw
		// span (for example a malformed `HostId ""`) before inserting a new ID.
		if (token.end > token.start)
			tokens.append(token);
	}
	return tokens;
}

void removeFilterParameterPairs(
	QString& text,
	int parametersOffset,
	const QList<FilterParameterToken>& tokens,
	const QList<int>& keyTokenIndices)
{
	for (auto it = keyTokenIndices.crbegin(); it != keyTokenIndices.crend(); ++it)
	{
		const int keyIndex = *it;
		const int start = parametersOffset + tokens[keyIndex].start;
		const int end = parametersOffset + (keyIndex + 1 < tokens.size()
			? tokens[keyIndex + 1].end
			: tokens[keyIndex].end);
		text.remove(start, end - start);
	}
}

QString cloneFilterLineWithNewInstanceIds(const QString& sourceText)
{
	QString clonedText = sourceText;
	const int separator = sourceText.indexOf(':');
	if (separator < 0)
		return clonedText;

	QString command = sourceText.left(separator).trimmed();
	if (command.startsWith('#'))
		command = command.mid(1).trimmed();
	const QString parameters = sourceText.mid(separator + 1);
	const QList<FilterParameterToken> tokens =
		tokenizeFilterParameters(parameters);

	if (command == QStringLiteral("OutProcVSTPlugin"))
	{
		QList<int> hostIdTokenIndices;
		std::vector<std::wstring> tokenValues;
		tokenValues.reserve(static_cast<std::size_t>(tokens.size()));
		for (const FilterParameterToken& token : tokens)
			tokenValues.push_back(token.value.toStdWString());

		for (std::size_t i = 0; i < tokenValues.size();)
		{
			const std::wstring& key = tokenValues[i];
			if (key == L"Library" || key == L"ChunkData" ||
				key == L"ClassIndex" || key == L"MidiConfig" ||
				key == L"Engine" || key == L"HostId")
			{
				if (key == L"HostId")
					hostIdTokenIndices.append(static_cast<int>(i));
				i += i + 1 < tokenValues.size() ? 2 : 1;
				continue;
			}
			if (i + 1 >= tokenValues.size())
				break;

			float value = 0.0f;
			if (VSTParseFiniteFloat(tokenValues[i + 1], value))
			{
				i += 2;
				continue;
			}
			if (VSTParseLegacyParameterIndex(key) &&
				i + 2 < tokenValues.size() &&
				VSTParseFiniteFloat(tokenValues[i + 2], value))
			{
				i += 3;
				continue;
			}
			++i;
		}

		removeFilterParameterPairs(
			clonedText, separator + 1, tokens, hostIdTokenIndices);
		// Put the authoritative pair first. Even if the source row contains some
		// other malformed/dangling parameter, it cannot consume the fresh ID.
		clonedText.insert(separator + 1, " HostId " +
			QUuid::createUuid().toString(QUuid::WithoutBraces) + " ");
	}
	else if (command == QStringLiteral("VUMeter"))
	{
		QList<int> meterIdTokenIndices;
		for (int i = 0; i < tokens.size(); i += 2)
		{
			if (tokens[i].value.compare(
				QStringLiteral("MeterId"), Qt::CaseInsensitive) == 0)
			{
				meterIdTokenIndices.append(i);
			}
		}
		removeFilterParameterPairs(
			clonedText, separator + 1, tokens, meterIdTokenIndices);
		clonedText.insert(separator + 1, " MeterId " +
			QUuid::createUuid().toString(QUuid::WithoutBraces) + " ");
	}

	return clonedText;
}
}
#include "FilterTableRow.h"
#include "FilterTableMimeData.h"
#include "guis/ExpressionFilterGUIFactory.h"
#include "guis/CommentFilterGUIFactory.h"
#include "guis/DeviceFilterGUIFactory.h"
#include "guis/ChannelFilterGUIFactory.h"
#include "guis/StageFilterGUIFactory.h"
#include "guis/PreampFilterGUIFactory.h"
#include "guis/BiQuadFilterGUIFactory.h"
#include "guis/ParametricEQFilterGUIFactory.h"
#include "guis/CopyFilterGUIFactory.h"
#include "guis/DelayFilterGUIFactory.h"
#include "guis/IncludeFilterGUIFactory.h"
#include "guis/GraphicEQFilterGUIFactory.h"
#include "guis/ConvolutionFilterGUIFactory.h"
#include "guis/HeadphoneCalibrationFilterGUIFactory.h"
#include "guis/VSTPluginFilterGUIFactory.h"
#include "guis/LoudnessCorrectionFilterGUIFactory.h"
#include "guis/OriginalLoudnessCorrectionFilterGUIFactory.h"
#include "guis/AudioToolFilterGUIFactory.h"
#include "Editor/helpers/GUIHelper.h"
#include "helpers/StringHelper.h"
#include "helpers/LogHelper.h"
#include "helpers/ChannelHelper.h"
#include "helpers/RegistryHelper.h"
#include "FilterTable.h"

using namespace std;

namespace
{
	class ThemeIconLabel final : public QLabel
	{
	public:
		ThemeIconLabel(GUIHelper::ThemeIcon themeIcon, const QSize& iconSize, QWidget* parent)
			: QLabel(parent), icon(GUIHelper::createThemeIcon(themeIcon))
		{
			setFixedSize(iconSize);
		}

	protected:
		void paintEvent(QPaintEvent* event) override
		{
			QLabel::paintEvent(event);
			QPainter painter(this);
			icon.paint(&painter, rect(), Qt::AlignCenter,
				isEnabled() ? QIcon::Normal : QIcon::Disabled);
		}

	private:
		QIcon icon;
	};

	void configureFilterLayout(QGridLayout* layout)
	{
		const int margin = GUIHelper::scale(7);
		layout->setContentsMargins(
			margin,
			margin,
			margin,
			GUIHelper::scale(9));
		layout->setHorizontalSpacing(0);
		layout->setVerticalSpacing(GUIHelper::scale(5));
		layout->setColumnStretch(0, 1);
		layout->setColumnStretch(1, 0);
	}
}

FilterTable::FilterTable(MainWindow* mainWindow, QWidget* parent)
	: QWidget(parent), mainWindow(mainWindow)
{
	setObjectName(QStringLiteral("filterTable"));
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	setMinimumWidth(0);
	gridLayout = new QGridLayout(this);
	configureFilterLayout(gridLayout);

	insertArrow = new ThemeIconLabel(
		GUIHelper::ThemeIcon::ArrowRight,
		GUIHelper::scale(QSize(24, 15)),
		this);
	insertArrow->setVisible(false);

	factories.append(new ExpressionFilterGUIFactory);
	factories.append(new CommentFilterGUIFactory);
	factories.append(new IncludeFilterGUIFactory);
	factories.append(new DeviceFilterGUIFactory);
	factories.append(new ChannelFilterGUIFactory);
	factories.append(new StageFilterGUIFactory);
	factories.append(new PreampFilterGUIFactory);
	factories.append(new BiQuadFilterGUIFactory);
	factories.append(new DelayFilterGUIFactory);
	factories.append(new CopyFilterGUIFactory);
	factories.append(new PanFilterGUIFactory);
	factories.append(new CrossfeedFilterGUIFactory);
	factories.append(new ChorusFilterGUIFactory);
	factories.append(new ReverbFilterGUIFactory);
	factories.append(new ToneGeneratorFilterGUIFactory);
	factories.append(new VUMeterFilterGUIFactory);
	factories.append(new HeadphoneCalibrationFilterGUIFactory);
	factories.append(new GraphicEQFilterGUIFactory);
	factories.append(new ParametricEQFilterGUIFactory);
	factories.append(new ConvolutionFilterGUIFactory);
	factories.append(new VSTPluginFilterGUIFactory);
	factories.append(new LoudnessCorrectionFilterGUIFactory);
	factories.append(new OriginalLoudnessCorrectionFilterGUIFactory);

	QApplication::instance()->installEventFilter(this);
}

FilterTable::~FilterTable()
{
	qDeleteAll(items);
	items.clear();
	for (IFilterGUIFactory* factory : factories)
		delete factory;
	factories.clear();
}

void FilterTable::initialize(QScrollArea* scrollArea, const QList<shared_ptr<AbstractAPOInfo>>& outputDevices, const QList<shared_ptr<AbstractAPOInfo>>& inputDevices)
{
	this->scrollArea = scrollArea;
	this->outputDevices = outputDevices;
	this->inputDevices = inputDevices;

	for (IFilterGUIFactory* factory : factories)
		factory->initialize(this);
}

void FilterTable::updateDeviceAndChannelMask(shared_ptr<AbstractAPOInfo> selectedDevice, int channelMask)
{
	this->selectedDevice = selectedDevice;
	this->selectedChannelMask = channelMask;

	if (!items.empty())
		updateGuis();
}

void FilterTable::updateGuis()
{
	QElapsedTimer timer;
	timer.start();

	for (Item* item : items)
	{
		if (item->gui != NULL)
		{
			item->prefs.clear();
			item->gui->storePreferences(item->prefs);
			item->runtimeState.clear();
			item->gui->takeRuntimeState(item->runtimeState);
		}
	}

	delete layout();

	for (QObject* object : children())
	{
		if (object != insertArrow)
		{
			QWidget* widget = qobject_cast<QWidget*>(object);
			if (widget != NULL)
				widget->setVisible(false);
			object->deleteLater();
		}
	}

	qDebug("Delete took %d ms", timer.elapsed());
	timer.start();

	gridLayout = new QGridLayout(this);
	configureFilterLayout(gridLayout);

	for (IFilterGUIFactory* factory : factories)
		factory->startOfFile(configPath);

	int row = 0;
	int minimumContentHeight = 0;
	for (Item* item : items)
	{
		QString line = item->text;
		IFilterGUI* gui = NULL;
		int pos = line.indexOf(':');
		if (pos != -1)
		{
			QString key = line.mid(0, pos);
			QString value = line.mid(pos + 1);

			// allow to use indentation
			key = key.trimmed();
			QString factoryKey = key;
			QString factoryValue = value;

			for (IFilterGUIFactory* factory : factories)
			{
				gui = factory->createFilterGUI(factoryKey, factoryValue);

				if (gui != NULL || factoryKey == "")
					break;
			}

			if (gui != NULL)
			{
				for (IFilterGUIFactory* factory : factories)
				{
					gui = factory->decorateFilterGUI(gui);
				}
			}
		}

		FilterTableRow* rowWidget = new FilterTableRow(this, row + 1, item, gui);
		gridLayout->addWidget(rowWidget, row, 0);

		item->gui = gui;

		if (gui != NULL)
		{
			gui->loadPreferences(item->prefs);
			gui->restoreRuntimeState(item->runtimeState);
			item->runtimeState.clear();

			connect(gui, SIGNAL(updateModel()), this, SLOT(updateModel()));
			connect(gui, SIGNAL(updateChannels()), this, SLOT(updateChannels()));
		}

		const int rowHeight = rowWidget->minimumSizeHint().height();
		rowWidget->setMinimumHeight(rowHeight);
		gridLayout->setRowMinimumHeight(row, rowHeight);
		gridLayout->setRowStretch(row, 0);
		minimumContentHeight += rowHeight;
		row++;
	}

	for (IFilterGUIFactory* factory : factories)
		factory->endOfFile(configPath);

	propagateChannels();

	QToolBar* toolBar = new QToolBar;
	toolBar->setObjectName(QStringLiteral("filterAddBar"));
	toolBar->setIconSize(GUIHelper::scale(QSize(16, 16)));

	QWidget* spacer = new QWidget;
	spacer->setFixedWidth(GUIHelper::scale(38));
	toolBar->addWidget(spacer);

	QAction* addAction = new QAction(GUIHelper::createAccentAddIcon(), tr("Add filter"), toolBar);
	addAction->setCheckable(true);
	connect(addAction, SIGNAL(triggered()), this, SLOT(addActionTriggered()));
	toolBar->addAction(addAction);

	gridLayout->addWidget(toolBar, row++, 0, 1, 1, Qt::AlignLeft | Qt::AlignTop);
	minimumContentHeight += toolBar->sizeHint().height();

	QSpacerItem* spacerItem = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
	gridLayout->addItem(spacerItem, row, 0);

	gridLayout->setRowStretch(row, 1);
	setMinimumHeight(minimumContentHeight);

	disableWheelForWidgets();

	qDebug("Create took %d ms", timer.elapsed());
	update();
}

void FilterTable::propagateChannels()
{
	vector<wstring> channelNames;
	if (selectedDevice != NULL)
		channelNames = ChannelHelper::getChannelNames(selectedDevice->getChannelCount(), selectedChannelMask);

	for (Item* item : items)
	{
		if (item->gui != NULL)
			item->gui->configureChannels(channelNames);
	}
}

QList<QString> FilterTable::getLines()
{
	QList<QString> result;
	for (Item* item : items)
		result.append(item->text);

	return result;
}

bool FilterTable::makeLinesWithGuiOverride(
	IFilterGUI* target,
	const QString& command,
	const QString& parameters,
	QList<QString>* lines) const
{
	if (target == NULL || lines == NULL || command.trimmed().isEmpty())
		return false;

	lines->clear();
	bool replaced = false;
	for (Item* item : items)
	{
		if (item->gui != NULL &&
			(item->gui == target || item->gui->isAncestorOf(target)))
		{
			lines->append(command + ": " + parameters);
			replaced = true;
		}
		else
		{
			lines->append(item->text);
		}
	}
	return replaced;
}

bool FilterTable::planPreampReduction(
	double reductionDb,
	PreampAdjustmentPlan* plan) const
{
	if (plan == NULL || !std::isfinite(reductionDb) || reductionDb <= 0.0)
		return false;
	*plan = PreampAdjustmentPlan();

	bool hasScopeBoundary = false;
	for (Item* item : items)
	{
		if (isPreampScopeBoundary(item->text))
		{
			hasScopeBoundary = true;
			break;
		}
	}

	for (int index = 0; index < items.size(); ++index)
	{
		const QString line = items[index]->text;
		const QString trimmed = line.trimmed();
		if (trimmed.isEmpty() || trimmed.startsWith('#'))
			continue;
		const int separator = trimmed.indexOf(':');
		if (separator < 0)
			continue;
		const QString command = trimmed.left(separator).trimmed();
		if (preampScopeBoundaries().contains(command.toCaseFolded()))
			break;
		if (command != QStringLiteral("Preamp"))
			continue;
		double oldDbGain = 0.0;
		if (!parseEditablePreampLine(line, nullptr, &oldDbGain, nullptr))
			continue;

		// The line is serialized with two decimal places. Round the final target
		// toward attenuation as well as rounding the measured excess, otherwise
		// an existing value with finer precision could round back upward and leave
		// a small positive response peak.
		const double unquantizedTarget = oldDbGain - reductionDb;
		double targetDbGain = std::floor(unquantizedTarget * 100.0) / 100.0;
		QString targetText = QString::number(targetDbGain, 'f', 2);
		targetDbGain = targetText.toDouble();
		// Guard the exact cent boundary against floating-point multiplication or
		// formatting rounding upward. One extra cent is preferable to any positive
		// residual in the sampled response.
		if (oldDbGain - targetDbGain < reductionDb)
		{
			targetText = QString::number(targetDbGain - 0.01, 'f', 2);
			targetDbGain = targetText.toDouble();
		}
		if (std::isfinite(targetDbGain)
			&& targetDbGain >= -100.0 && targetDbGain <= 100.0
			&& targetDbGain < oldDbGain
			&& oldDbGain - targetDbGain >= reductionDb)
		{
			plan->insertsNewPreamp = false;
			plan->oldDbGain = oldDbGain;
			plan->targetDbGain = targetDbGain;
			plan->itemIndex = index;
			plan->originalLine = line;
			return true;
		}
		// A root Preamp that is already at its editable floor can still be made
		// safe by another editable root Preamp later in the file. Keep searching;
		// if none can represent the cut, insertion is considered below.
		continue;
	}

	// Inserting before Device/Channel/If/Include/Stage would broaden a cut from
	// one conditional path to the entire file. Require a deliberate manual edit
	// instead of changing unrelated devices or channels.
	if (hasScopeBoundary)
		return false;

	plan->insertsNewPreamp = true;
	plan->oldDbGain = 0.0;
	plan->targetDbGain = QString::number(
		std::floor(-reductionDb * 100.0) / 100.0, 'f', 2).toDouble();
	if (-plan->targetDbGain < reductionDb)
		plan->targetDbGain = QString::number(
			plan->targetDbGain - 0.01, 'f', 2).toDouble();
	plan->itemIndex = -1;
	plan->originalLine.clear();
	// Keep newly inserted values within the editable range of the standard
	// Preamp control.  Refuse instead of silently clipping the requested cut.
	return std::isfinite(plan->targetDbGain)
		&& plan->targetDbGain >= -100.0 && plan->targetDbGain < 0.0
		&& -plan->targetDbGain >= reductionDb;
}

bool FilterTable::applyPreampReduction(const PreampAdjustmentPlan& plan)
{
	if (!std::isfinite(plan.oldDbGain) || !std::isfinite(plan.targetDbGain)
		|| plan.targetDbGain < -100.0 || plan.targetDbGain > 100.0)
		return false;

	if (!plan.insertsNewPreamp)
	{
		if (plan.itemIndex < 0 || plan.itemIndex >= items.size()
			|| items[plan.itemIndex]->text != plan.originalLine)
			return false;
		for (int index = 0; index <= plan.itemIndex; ++index)
			if (isPreampScopeBoundary(items[index]->text))
				return false;

		QString prefix;
		QString suffix;
		double parsedOldGain = 0.0;
		bool commaDecimal = false;
		if (!parseEditablePreampLine(
			plan.originalLine, &prefix, &parsedOldGain, &suffix,
			&commaDecimal))
			return false;
		QString targetText = QString::number(plan.targetDbGain, 'f', 2);
		bool validTargetGain = false;
		const double serializedTargetGain = targetText.toDouble(&validTargetGain);
		if (!validTargetGain
			|| !std::isfinite(parsedOldGain) || !std::isfinite(serializedTargetGain)
			|| parsedOldGain != plan.oldDbGain
			|| serializedTargetGain != plan.targetDbGain
			|| serializedTargetGain >= parsedOldGain)
			return false;
		if (commaDecimal)
			targetText.replace('.', ',');
		items[plan.itemIndex]->text = prefix + targetText + suffix;
		updateGuis();
		updateModel();
		return true;
	}

	if (plan.itemIndex != -1 || !plan.originalLine.isEmpty()
		|| plan.oldDbGain != 0.0 || plan.targetDbGain >= 0.0)
		return false;
	const QString serializedTargetText = QString::number(
		plan.targetDbGain, 'f', 2);
	bool validSerializedTarget = false;
	const double serializedTargetGain = serializedTargetText.toDouble(
		&validSerializedTarget);
	if (!validSerializedTarget || !std::isfinite(serializedTargetGain)
		|| serializedTargetGain != plan.targetDbGain
		|| serializedTargetGain >= 0.0)
		return false;
	for (Item* item : items)
		if (isPreampScopeBoundary(item->text))
			return false;

	Item* before = items.isEmpty() ? NULL : items.first();
	addLine(
		QStringLiteral("Preamp: %1 dB").arg(plan.targetDbGain, 0, 'f', 2),
		before);
	updateGuis();
	return true;
}

bool FilterTable::processingPrecisionEditable() const
{
	// Do not infer the effective engine format from one tab when conditional
	// scopes or included files can contribute another active declaration.
	for (const Item* item : items)
	{
		const QString key = item->text.section(':', 0, 0).trimmed();
		if (key == "Include"
			|| key == "If" || key == "ElseIf" || key == "Else" || key == "EndIf"
			|| key == "Eval" || key == "Expression") return false;
	}
	return true;
}

bool FilterTable::setDoublePrecision(bool enabled)
{
	if (!processingPrecisionEditable()) return false;
	Item* precisionItem = nullptr;
	for (Item* item : items)
	{
		const QString text = item->text.trimmed();
		if (text.section(':', 0, 0).trimmed() != QStringLiteral("ProcessingPrecision")) continue;
		const QString value = text.mid(text.indexOf(':') + 1).trimmed();
		if (precisionItem || (value != "32" && value != "64")) return false;
		precisionItem = item;
	}
	const QString line = QStringLiteral("ProcessingPrecision: %1").arg(enabled ? 64 : 32);
	if (precisionItem)
	{
		if (!replaceItemText(precisionItem, line)) return false;
	}
	else
	{
		QSignalBlocker blocker(this);
		addLine(line, items.isEmpty() ? nullptr : items.first());
	}
	updateGuis();
	updateModel();
	return true;
}

bool FilterTable::setLines(const QString& configPath, const QList<QString>& lines)
{
	// Preflight is intentionally side-effect free. External resources are
	// released only immediately before this table mutation.
	if (!prepareDeleteAllItems() || !commitDeleteItems(items))
		return false;
	setLinesAfterDeleteCommit(configPath, lines);
	return true;
}

void FilterTable::setLinesAfterDeleteCommit(
	const QString& configPath,
	const QList<QString>& lines)
{
	this->configPath = configPath;

	selected.clear();
	focused = NULL;
	selectionStart = NULL;
	internalDrag = false;
	dragStartPos = QPoint();
	if (insertArrow != NULL)
		insertArrow->hide();
	qDeleteAll(items);
	items.clear();

	for (QString line : lines)
	{
		items.append(new Item(line));
	}

	setScrollOffsets(0, 0);
	if (!configPath.isEmpty())
	{
		QSettings settings(QString::fromWCharArray(EDITOR_PER_FILE_REGPATH), QSettings::NativeFormat);
		settings.beginGroup(QString(configPath).replace('\\', '|'));
		QVariant prefsValue = settings.value("rowPrefs");
		QStringList prefLines;
		if (prefsValue.isValid())
			prefLines = prefsValue.toStringList();
		for (const QString& prefLine : prefLines)
		{
			const int index = prefLine.indexOf(':');
			if (index == -1)
				continue;

			bool validLineNumber = false;
			const int lineNumber = prefLine.left(index).toInt(&validLineNumber);
			if (!validLineNumber || lineNumber <= 0 || lineNumber > items.size())
				continue;

			const int index2 = prefLine.indexOf(':', index + 1);
			if (index2 != -1)
			{
				const QString prefCommand = prefLine.mid(index + 1, index2 - index - 1);
				const QString prefString = prefLine.mid(index2 + 1);
				Item* item = items[lineNumber - 1];

				QString command;
				const int commandSeparator = item->text.indexOf(':');
				if (commandSeparator != -1)
					command = item->text.left(commandSeparator).trimmed();

				if (command == prefCommand)
					item->prefs = QJsonDocument::fromJson(prefString.toUtf8()).toVariant().toMap();
			}
		}
		setScrollOffsets(settings.value("scrollX", 0).toInt(), settings.value("scrollY", 0).toInt());
		settings.endGroup();
	}

	if (!items.isEmpty())
	{
		focused = items[0];
		selectionStart = items[0];
	}
	else
	{
		focused = NULL;
		selectionStart = NULL;
	}

	updateGuis();
}

FilterTable::Item* FilterTable::addLine(const QString& line, FilterTable::Item* before)
{
	Item* newItem = new Item(line);

	if (before != NULL)
	{
		int index = items.indexOf(before);
		items.insert(index, newItem);
	}
	else
	{
		items.append(newItem);
	}

	emit linesChanged();

	return newItem;
}

FilterTable::Item* FilterTable::cloneItem(FilterTable::Item* item, bool insertBelow)
{
	const int sourceIndex = items.indexOf(item);
	if (sourceIndex < 0)
		return NULL;

	if (item->gui != NULL)
	{
		item->prefs.clear();
		item->gui->storePreferences(item->prefs);
	}

	const QString clonedText = cloneFilterLineWithNewInstanceIds(item->text);

	Item* clone = new Item(clonedText);
	clone->prefs = item->prefs;
	items.insert(sourceIndex + (insertBelow ? 1 : 0), clone);

	selected.clear();
	selected.insert(clone);
	focused = clone;
	selectionStart = clone;

	emit linesChanged();
	return clone;
}

bool FilterTable::replaceItemText(FilterTable::Item* item, const QString& text)
{
	if (item == NULL || !items.contains(item))
		return false;
	if (item->text == text)
		return true;
	if (!prepareItemReplacement(item) || !commitDeleteItem(item))
		return false;

	item->text = text;
	return true;
}

bool FilterTable::prepareItemReplacement(FilterTable::Item* item)
{
	return item != NULL && items.contains(item) && prepareDeleteItem(item);
}

bool FilterTable::removeItem(FilterTable::Item* item)
{
	if (item == NULL || !items.contains(item) || !prepareDeleteItem(item) ||
		!commitDeleteItem(item))
		return false;

	items.removeOne(item);
	selected.remove(item);
	if (focused == item)
		focused = NULL;
	if (selectionStart == item)
		selectionStart = NULL;
	delete item;
	emit linesChanged();
	return true;
}

bool FilterTable::prepareDeleteItem(FilterTable::Item* item)
{
	return item == NULL || item->gui == NULL || item->gui->prepareDelete();
}

bool FilterTable::commitDeleteItem(FilterTable::Item* item)
{
	return item == NULL || item->gui == NULL || item->gui->commitDelete();
}

bool FilterTable::prepareDeleteItems(const QList<FilterTable::Item*>& candidateItems)
{
	for (Item* item : candidateItems)
	{
		if (!prepareDeleteItem(item))
			return false;
	}
	return true;
}

bool FilterTable::prepareDeleteAllItems()
{
	return prepareDeleteItems(items);
}

bool FilterTable::commitDeleteItems(
	const QList<FilterTable::Item*>& candidateItems)
{
	for (Item* item : candidateItems)
	{
		if (!commitDeleteItem(item))
			return false;
	}
	return true;
}

bool FilterTable::commitDeleteAllItems()
{
	return prepareDeleteItems(items) && commitDeleteItems(items);
}

QMenu* FilterTable::createAddPopupMenu()
{
	QHash<QList<QString>, QMenu*> pathMap;
	QMenu* rootMenu = new QMenu(this);
	pathMap[QStringList()] = rootMenu;

	for (IFilterGUIFactory* f : factories)
	{
		QList<FilterTemplate> templates = f->createFilterTemplates();
		for (FilterTemplate t : templates)
		{
			QMenu* menu = pathMap.value(t.getPath());
			if (menu == NULL)
			{
				QMenu* parentMenu = rootMenu;
				QStringList currentPath;
				for (QString pathSegment : t.getPath())
				{
					currentPath.append(pathSegment);
					menu = pathMap.value(currentPath);
					if (menu == NULL)
					{
						menu = new QMenu(pathSegment, parentMenu);
						pathMap.insert(currentPath, menu);
						parentMenu->addMenu(menu);
					}
					parentMenu = menu;
				}
			}

			QAction* action = menu->addAction(t.getName());
			action->setData(QVariant::fromValue(t));
		}
	}

	return rootMenu;
}

void FilterTable::cut()
{
	const QList<Item*> itemsToCut = selectedItemsInOrder();
	if (itemsToCut.isEmpty() || !prepareDeleteItems(itemsToCut))
		return;
	if (!commitDeleteItems(itemsToCut))
		return;

	// commitDelete() may synchronously recover newer VST state into Item::text.
	// Build the clipboard only after that succeeds, then consume that exact set.
	copyItemsToClipboard(itemsToCut);
	deletePreparedItems(itemsToCut);
}

void FilterTable::copy()
{
	copyItemsToClipboard(selectedItemsInOrder());
}

QList<FilterTable::Item*> FilterTable::selectedItemsInOrder() const
{
	QList<Item*> result;
	for (Item* item : items)
	{
		if (selected.contains(item))
			result.append(item);
	}
	return result;
}

void FilterTable::populateMimeDataFromItems(
	FilterTableMimeData* mimeData,
	const QList<Item*>& sourceItems)
{
	if (mimeData == NULL)
		return;
	QString text;
	QList<QVariantMap> prefsList;
	bool first = true;
	for (Item* item : sourceItems)
	{
		if (first)
			first = false;
		else
			text += "\n";
		text += item->text;
		if (item->gui != NULL)
			item->gui->storePreferences(item->prefs);
		prefsList.append(item->prefs);
	}
	mimeData->setText(text);
	mimeData->setPrefsList(prefsList);
}

void FilterTable::copyItemsToClipboard(const QList<Item*>& itemsToCopy)
{
	if (!itemsToCopy.isEmpty())
	{
		FilterTableMimeData* mimeData = new FilterTableMimeData;
		populateMimeDataFromItems(mimeData, itemsToCopy);
		QClipboard* clipboard = QApplication::clipboard();
		clipboard->setMimeData(mimeData);
	}
}

void FilterTable::paste()
{
	QClipboard* clipboard = QApplication::clipboard();
	const QMimeData* mimeData = clipboard->mimeData();
	if (mimeData->hasText())
	{
		int dropRow = items.size();
		for (int i = 0; i < items.size(); i++)
		{
			if (selected.contains(items[i]))
			{
				dropRow = i;
				break;
			}
		}

		QString text = mimeData->text();
		QStringList textLines = text.split("\n");
		QList<QVariantMap> prefsList;
		const FilterTableMimeData* filterTableMimeData = qobject_cast<const FilterTableMimeData*>(mimeData);
		if (filterTableMimeData != NULL)
			prefsList = filterTableMimeData->getPrefsList();

		selected.clear();
		focused = NULL;
		selectionStart = NULL;
		for (int i = 0; i < textLines.size(); i++)
		{
			const QString line = cloneFilterLineWithNewInstanceIds(textLines[i]);
			Item* item = new Item(line);
			if (!prefsList.isEmpty())
				item->prefs = prefsList[i];
			selected.insert(item);
			items.insert(dropRow++, item);
			if (focused == NULL)
			{
				focused = item;
				selectionStart = item;
			}
		}

		emit linesChanged();
		updateGuis();
	}
}

bool FilterTable::deleteSelectedLines()
{
	const QList<Item*> itemsToDelete = selectedItemsInOrder();
	if (itemsToDelete.isEmpty())
		return true;
	if (!prepareDeleteItems(itemsToDelete) ||
		!commitDeleteItems(itemsToDelete))
		return false;
	deletePreparedItems(itemsToDelete);
	return true;
}

void FilterTable::deletePreparedItems(const QList<Item*>& itemsToDelete)
{
	QSet<Item*> deleteSet;
	for (Item* item : itemsToDelete)
		deleteSet.insert(item);

	QList<Item*> newItems;
	for (Item* item : items)
	{
		if (deleteSet.contains(item))
		{
			if (item == focused)
				focused = NULL;
			if (item == selectionStart)
				selectionStart = NULL;
			delete item;
		}
		else
		{
			newItems.append(item);
		}
	}
	selected.clear();
	items = newItems;
	emit linesChanged();
	updateGuis();
}

void FilterTable::selectAll()
{
	selected.clear();
	for (Item* item : items)
		selected.insert(item);
	update();
}

int FilterTable::findText(const QString& text, bool backwards)
{
	if (text.trimmed().isEmpty() || items.isEmpty())
		return -1;

	const int direction = backwards ? -1 : 1;
	int row = items.indexOf(focused);
	if (row < 0)
		row = backwards ? 0 : -1;

	for (int checked = 0; checked < items.size(); ++checked)
	{
		row = (row + direction + items.size()) % items.size();
		if (!items[row]->text.contains(text, Qt::CaseInsensitive))
			continue;

		selected.clear();
		selected.insert(items[row]);
		focused = items[row];
		selectionStart = items[row];
		ensureRowVisible(row);
		update();
		return row;
	}

	return -1;
}

void FilterTable::clearFindSelection()
{
	selected.clear();
	selectionStart = focused;
	update();
}

void FilterTable::updateModel()
{
	emit linesChanged();
}

void FilterTable::updateChannels()
{
	propagateChannels();
}

void FilterTable::addActionTriggered()
{
	std::unique_ptr<QMenu> menu(createAddPopupMenu());
	QAction* addAction = qobject_cast<QAction*>(QObject::sender());
	QToolBar* toolBar = addAction != NULL ? qobject_cast<QToolBar*>(addAction->parent()) : NULL;
	if (addAction == NULL || toolBar == NULL)
		return;

	QRect rect = toolBar->actionGeometry(addAction);
	QPoint p = toolBar->mapToGlobal(QPoint(rect.x(), rect.y() + rect.height()));
	QAction* action = menu->exec(p);
	addAction->setChecked(false);
	if (action != NULL)
	{
		FilterTemplate t = action->data().value<FilterTemplate>();
		QString line = t.getLine();
		addLine(line);
		updateGuis();
	}
}

void FilterTable::openConfig(QString path)
{
	mainWindow->load(path);
}

QSize FilterTable::minimumSizeHint() const
{
	QSize size = QWidget::minimumSizeHint();
	// Filter GUIs may have wide preferred layouts, but the outer scroll area
	// must stay viewport-sized; individual controls are responsible for their
	// own compression or internal scrolling.
	size.setWidth(0);
	if (size.height() < minimumHeightHint)
		size.setHeight(minimumHeightHint);

	return size;
}

void FilterTable::setMinimumHeightHint(int height)
{
	minimumHeightHint = height;
	updateGeometry();
}

void FilterTable::savePreferences()
{
	if (!configPath.isEmpty())
	{
		QStringList prefLines;

		for (int i = 0; i < items.size(); i++)
		{
			Item* item = items[i];

			if (item->gui != NULL)
			{
				item->prefs.clear();
				item->gui->storePreferences(item->prefs);
			}

			if (!item->prefs.isEmpty())
			{
				QString command;
				int index = item->text.indexOf(':');
				if (index != -1)
					command = item->text.left(index).trimmed();

				QByteArray byteArray = QJsonDocument::fromVariant(item->prefs).toJson(QJsonDocument::Compact);
				QString string = QString("%1:%2:%3").arg(i + 1).arg(command).arg(QString::fromUtf8(byteArray));
				prefLines.append(string);
			}
		}

		QSettings settings(QString::fromWCharArray(EDITOR_PER_FILE_REGPATH), QSettings::NativeFormat);
		settings.beginGroup(QString(configPath).replace('\\', '|'));
		settings.setValue("rowPrefs", prefLines);
		settings.setValue("scrollX", scrollArea->horizontalScrollBar()->value());
		settings.setValue("scrollY", scrollArea->verticalScrollBar()->value());
		settings.endGroup();
	}
}

void FilterTable::setScrollOffsets(int x, int y)
{
	presetScrollX = x;
	presetScrollY = y;
}

void FilterTable::updateAnalysis()
{
	if (isVisible())
		mainWindow->startAnalysis();
}

void FilterTable::mousePressEvent(QMouseEvent* event)
{
	if (event->buttons() & Qt::LeftButton)
	{
		int row = rowForPos(event->position().toPoint(), false);
		if (row != -1)
		{
			Item* item = items[row];

			if (event->modifiers() & Qt::ControlModifier)
			{
				if (!selected.remove(item))
					selected.insert(item);
				selectionStart = item;
			}
			else if (event->modifiers() & Qt::ShiftModifier)
			{
				int startRow = items.indexOf(selectionStart);
				if (startRow != -1)
				{
					selected.clear();
					for (int i = min(startRow, row); i <= max(startRow, row); i++)
					{
						selected.insert(items[i]);
					}
				}
			}
			else
			{
				if (!selected.contains(item))
				{
					selected.clear();
					selected.insert(item);
				}
				selectionStart = item;
			}
			focused = item;
			ensureRowVisible(row);
			update();

			dragStartPos = event->position().toPoint();
		}
		else
		{
			selected.clear();
			update();
		}
	}
}

void FilterTable::mouseReleaseEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton)
	{
		int row = rowForPos(event->position().toPoint(), false);
		if (row != -1)
		{
			Item* item = items[row];

			if (!(event->modifiers() & Qt::ControlModifier) && !(event->modifiers() & Qt::ShiftModifier))
			{
				if (selected.contains(item) && selectionStart == item)
				{
					selected.clear();
					selected.insert(item);
				}
			}
			ensureRowVisible(row);
			update();
		}
	}
}

void FilterTable::mouseMoveEvent(QMouseEvent* event)
{
	if (event->buttons() & Qt::LeftButton)
	{
		if ((event->position().toPoint() - dragStartPos).manhattanLength() >= QApplication::startDragDistance())
		{
			int i = 0;
			bool dragPosInside = false;
			for (Item* item : items)
			{
				if (selected.contains(item))
				{
					if (!dragPosInside)
					{
						FilterTableRow* tableRow = qobject_cast<FilterTableRow*>(gridLayout->itemAtPosition(i, 0)->widget());
						QRect rect = tableRow->getHeaderRect().translated(tableRow->pos());
						if (rect.contains(dragStartPos))
							dragPosInside = true;
					}
				}
				i++;
			}

			if (selected.size() > 0 && dragPosInside)
			{
				QList<Item*> itemsToDelete;
				for (Item* item : items)
				{
					if (selected.contains(item))
						itemsToDelete.append(item);
				}
				// This phase must remain side-effect free: the user can still cancel
				// the drag or choose Copy after it begins.
				if (!prepareDeleteItems(itemsToDelete))
				{
					dragStartPos = event->position().toPoint();
					QWidget::mouseMoveEvent(event);
					return;
				}

				FilterTableMimeData* mimeData = new FilterTableMimeData;
				populateMimeDataFromItems(mimeData, itemsToDelete);
				mimeData->setMoveCommitHandler(
					[this, itemsToDelete, mimeData]()
					{
						if (!prepareDeleteItems(itemsToDelete) ||
							!commitDeleteItems(itemsToDelete))
							return false;
						// A successful shutdown can recover newer VST state. Refresh
						// the payload before an in-process target inserts anything.
						populateMimeDataFromItems(mimeData, itemsToDelete);
						return true;
					});

				QDrag* drag = new QDrag(this);
				drag->setMimeData(mimeData);
				internalDrag = true;
				Qt::DropAction action = drag->exec(Qt::MoveAction | Qt::CopyAction);
				internalDrag = false;
				if (action == Qt::MoveAction)
				{
					// In-process targets invoke this before accepting their drop.
					// External targets cannot, so commit here before removing the
					// source. A failed commit always retains every source row.
					if (mimeData->commitMove())
						deletePreparedItems(itemsToDelete);
				}
				else if (action != Qt::IgnoreAction)
				{
					emit linesChanged();
					updateGuis();
				}
			}
		}
	}

	QWidget::mouseMoveEvent(event);
}

void FilterTable::dragEnterEvent(QDragEnterEvent* event)
{
	if (event->mimeData()->hasText())
	{
		if (event->modifiers() & Qt::ControlModifier)
			event->setDropAction(Qt::CopyAction);
		else
			event->setDropAction(Qt::MoveAction);
		event->accept();
	}

	QWidget::dragEnterEvent(event);
}

void FilterTable::dragMoveEvent(QDragMoveEvent* event)
{
	if (event->mimeData()->hasText())
	{
		if (event->modifiers() & Qt::ControlModifier)
			event->setDropAction(Qt::CopyAction);
		else
			event->setDropAction(Qt::MoveAction);

		int dropRow = rowForPos(event->position().toPoint(), true);

		QRect rect = gridLayout->itemAtPosition(dropRow == -1 ? gridLayout->rowCount() - 2 : dropRow, 0)->geometry();
		insertArrow->move(0, rect.top() - insertArrow->height() / 2 - gridLayout->verticalSpacing() / 2);

		insertArrow->raise();
		insertArrow->show();
	}

	QWidget::dragMoveEvent(event);
}

void FilterTable::dragLeaveEvent(QDragLeaveEvent* event)
{
	insertArrow->hide();
}

void FilterTable::dropEvent(QDropEvent* event)
{
	const QMimeData* mimeData = event->mimeData();
	if (mimeData->hasText())
	{
		if (event->modifiers() & Qt::ControlModifier)
			event->setDropAction(Qt::CopyAction);
		else
			event->setDropAction(Qt::MoveAction);

		const FilterTableMimeData* filterTableMimeData = qobject_cast<const FilterTableMimeData*>(mimeData);
		const bool cloneInstanceIds = event->dropAction() == Qt::CopyAction;
		if (!cloneInstanceIds && filterTableMimeData != NULL &&
			!filterTableMimeData->commitMove())
		{
			event->ignore();
			insertArrow->hide();
			return;
		}

		QString text = mimeData->text();
		QStringList textLines = text.split("\n");
		QList<QVariantMap> prefsList;
		if (filterTableMimeData != NULL)
			prefsList = filterTableMimeData->getPrefsList();

		int dropRow = rowForPos(event->position().toPoint(), true);
		if (dropRow == -1)
			dropRow = items.size();

		selected.clear();
		focused = NULL;
		selectionStart = NULL;
		for (int i = 0; i < textLines.size(); i++)
		{
			QString line = textLines[i];
			if (cloneInstanceIds)
				line = cloneFilterLineWithNewInstanceIds(line);
			Item* item = new Item(line);
			if (!prefsList.isEmpty())
				item->prefs = prefsList[i];
			selected.insert(item);
			items.insert(dropRow++, item);
			if (focused == NULL)
			{
				focused = item;
				selectionStart = item;
			}
		}
		event->accept();

		if (!internalDrag)
		{
			emit linesChanged();
			updateGuis();
		}
	}

	insertArrow->hide();

	QWidget::dropEvent(event);
}

void FilterTable::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up)
	{
		if (focused != NULL)
		{
			int row = items.indexOf(focused);
			if (row != -1)
			{
				int newRow = row;
				if (event->key() == Qt::Key_Down && row + 1 < items.size())
					newRow = row + 1;
				else if (event->key() == Qt::Key_Up && row - 1 >= 0)
					newRow = row - 1;

				if (newRow != row)
				{
					focused = items[newRow];
					if (event->modifiers() & Qt::ControlModifier)
					{
					}
					else if (event->modifiers() & Qt::ShiftModifier)
					{
						int startRow = items.indexOf(selectionStart);
						if (startRow != -1)
						{
							selected.clear();
							for (int i = min(startRow, newRow); i <= max(startRow, newRow); i++)
							{
								selected.insert(items[i]);
							}
						}
					}
					else
					{
						selected.clear();
						selected.insert(focused);
						selectionStart = focused;
					}

					ensureRowVisible(newRow);
					update();
				}
			}
		}
	}

	if (event->key() == Qt::Key_Space)
	{
		if (!(event->modifiers() & Qt::ControlModifier) || !selected.remove(focused))
			selected.insert(focused);
		update();
	}

	if (event->key() == Qt::Key_F2)
	{
		if (focused != NULL)
		{
			int rowIndex = items.indexOf(focused);
			if (rowIndex != -1)
			{
				QLayoutItem* layoutItem = gridLayout->itemAtPosition(rowIndex, 0);
				FilterTableRow* tableRow = qobject_cast<FilterTableRow*>(layoutItem->widget());
				if (tableRow != NULL)
					tableRow->editText();
			}
		}
	}

	if (event->key() == Qt::Key_Delete)
	{
		deleteSelectedLines();
	}
}

void FilterTable::wheelEvent(QWheelEvent* event)
{
	scrollingNow = true;
	scrollStartPoint = event->globalPosition();

	QWidget::wheelEvent(event);
}

bool FilterTable::eventFilter(QObject* obj, QEvent* event)
{
	QEvent::Type type = event->type();
	if (type == QEvent::MouseButtonRelease)
	{
		QWidget* widget = qobject_cast<QWidget*>(obj);
		if (widget != NULL && widget->property("_eqapoSuppressResetRelease").toBool())
		{
			widget->setProperty("_eqapoSuppressResetRelease", false);
			static_cast<QMouseEvent*>(event)->accept();
			return true;
		}
	}
	if (type == QEvent::MouseButtonDblClick)
	{
		QWidget* widget = qobject_cast<QWidget*>(obj);
		if (widget != NULL && isAncestorOf(widget))
		{
			auto consumeResetEvent = [widget, event]() {
				widget->setProperty("_eqapoSuppressResetRelease", true);
				static_cast<QMouseEvent*>(event)->accept();
				return true;
			};
			const QVariant defaultValue = widget->property("defaultValue");
			if (QDoubleSpinBox* spin = qobject_cast<QDoubleSpinBox*>(widget))
			{
				const double neutral = defaultValue.isValid() ? defaultValue.toDouble() : (spin->minimum() <= 0.0 && spin->maximum() >= 0.0 ? 0.0 : spin->minimum());
				spin->setValue(neutral);
				return consumeResetEvent();
			}
			if (QSpinBox* spin = qobject_cast<QSpinBox*>(widget))
			{
				const int neutral = defaultValue.isValid() ? defaultValue.toInt() : (spin->minimum() <= 0 && spin->maximum() >= 0 ? 0 : spin->minimum());
				spin->setValue(neutral);
				return consumeResetEvent();
			}
			if (QAbstractSlider* slider = qobject_cast<QAbstractSlider*>(widget))
			{
				if (qobject_cast<QScrollBar*>(widget) == NULL)
				{
					QObject* resetTarget = widget->property("resetTarget").value<QObject*>();
					const QVariant defaultTargetValue = widget->property("defaultTargetValue");
					if (resetTarget != NULL && defaultTargetValue.isValid())
					{
						if (QDoubleSpinBox* spin = qobject_cast<QDoubleSpinBox*>(resetTarget))
						{
							spin->setValue(defaultTargetValue.toDouble());
							return consumeResetEvent();
						}
						if (QSpinBox* spin = qobject_cast<QSpinBox*>(resetTarget))
						{
							spin->setValue(defaultTargetValue.toInt());
							return consumeResetEvent();
						}
					}
					const int neutral = defaultValue.isValid() ? defaultValue.toInt() : (slider->minimum() <= 0 && slider->maximum() >= 0 ? 0 : slider->minimum());
					slider->setValue(neutral);
					return consumeResetEvent();
				}
			}
		}
	}
	if (scrollingNow)
	{
		if (type == QEvent::Wheel)
		{
			QWheelEvent* wheelEvent = (QWheelEvent*)event;
			scrollStartPoint = wheelEvent->globalPosition();

			QWidget* widget = qobject_cast<QWidget*>(obj);
			if (widget != NULL)
			{
				if (isAncestorOf(widget))
				{
					QApplication::sendEvent(parent(), event);
					return true;
				}
			}
		}
		else if (type == QEvent::MouseMove)
		{
			QMouseEvent* mouseEvent = (QMouseEvent*)event;

			if ((mouseEvent->globalPosition().toPoint() - scrollStartPoint).manhattanLength() > GUIHelper::scale(30))
				scrollingNow = false;
		}
	}

	return false;
}

void FilterTable::showEvent(QShowEvent*)
{
	if (presetScrollX != -1)
	{
		scrollArea->horizontalScrollBar()->setValue(presetScrollX);
		presetScrollX = -1;
	}

	if (presetScrollY != -1)
	{
		scrollArea->verticalScrollBar()->setValue(presetScrollY);
		presetScrollY = -1;
	}
}

void FilterTable::ensureRowVisible(int row)
{
	QScrollBar* vScrollBar = scrollArea->verticalScrollBar();
	if (vScrollBar != NULL)
	{
		QRect rect = rowRect(row).toAlignedRect();
		if (rect.top() < vScrollBar->value())
			vScrollBar->setValue(max(0, rect.top()));
		else if (rect.bottom() + 1 > vScrollBar->value() + scrollArea->viewport()->height())
			vScrollBar->setValue(min(vScrollBar->maximum(), rect.bottom() + 1 - scrollArea->viewport()->height()));
	}
}

int FilterTable::rowForPos(QPoint pos, bool insert)
{
	int row = -1;
	for (int i = 0; i < gridLayout->rowCount() - 2; i++)
	{
		QRect rect = gridLayout->itemAtPosition(i, 0)->geometry();
		int y;
		if (insert)
			y = rect.center().y();
		else
			y = rect.bottom();

		if (pos.y() <= y)
		{
			row = i;
			break;
		}
	}

	return row;
}

QRectF FilterTable::rowRect(int row)
{
	QRectF rect = gridLayout->itemAtPosition(row, 0)->geometry();
	rect = rect.marginsAdded(QMarginsF(-1.5, -1.5, -1.5, -0.5));
	return rect;
}

void FilterTable::disableWheelForWidgets()
{
	QList<QWidget*> widgets = findChildren<QWidget*>();
	for (QWidget* widget : widgets)
	{
		if (qobject_cast<QComboBox*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QDial*>(widget))
		{
			widget->installEventFilter(new DisableWheelFilter(this, widget));
			if (widget->focusPolicy() == Qt::WheelFocus)
				widget->setFocusPolicy(Qt::StrongFocus);
		}
	}
}

QString FilterTable::getConfigPath() const
{
	return configPath;
}

void FilterTable::setConfigPath(const QString& value)
{
	configPath = value;
}

FilterTable::Item* FilterTable::getFocusedItem() const
{
	return focused;
}

const QSet<FilterTable::Item*>& FilterTable::getSelectedItems() const
{
	return selected;
}

const QList<shared_ptr<AbstractAPOInfo>>& FilterTable::getOutputDevices() const
{
	return outputDevices;
}

const QList<shared_ptr<AbstractAPOInfo>>& FilterTable::getInputDevices() const
{
	return inputDevices;
}

shared_ptr<AbstractAPOInfo> FilterTable::getSelectedDevice() const
{
	return selectedDevice;
}

int FilterTable::getSelectedChannelMask() const
{
	return selectedChannelMask;
}
