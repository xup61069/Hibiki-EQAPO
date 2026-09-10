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
#include <cmath>
#include <QEvent>
#include <QFileDialog>
#include <QScreen>
#include <QTimer>
#include <vector>
#include <QAbstractScrollArea>
#include <QMessageBox>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTextStream>
#define ENABLE_SNDFILE_WINDOWS_PROTOTYPES 1
#include <sndfile.h>

#include "helpers/GainIterator.h"
#include "DeviceAPOInfo.h"
#include "Editor/helpers/GUIHelper.h"
#include "Editor/widgets/ResizeCorner.h"
#include "Editor/FilterTable.h"
#include "GraphicEQFilterGUIView.h"
#include "GraphicEQFilterGUI.h"
#include "ui_GraphicEQFilterGUI.h"

static const double DEFAULT_TABLE_WIDTH = 150;
static const double DEFAULT_VIEW_HEIGHT = 150;
static const int TABLE_RESIZE_ORIGIN = 10000;

using namespace std;

QRegularExpression GraphicEQFilterGUI::numberRegEx("[-+]?[0-9]*\\.?[0-9]+([eE][-+]?[0-9]+)?");

GraphicEQFilterGUI::GraphicEQFilterGUI(GraphicEQFilter* filter, QString configPath, FilterTable* filterTable)
	: ui(new Ui::GraphicEQFilterGUI), scene(nullptr), configPath(configPath), filterTable(filterTable)
{
	ui->setupUi(this);
	ui->label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	QFont font = ui->label->font();
	font.setWeight(QFont::DemiBold);
	ui->label->setFont(font);
	if (filterTable != nullptr)
		filterTable->installEventFilter(this);
	ui->actionImport->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Import));
	ui->actionExport->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Export));
	updateThemeIcons();
	ui->tableWidget->horizontalHeader()->setMinimumSectionSize(GUIHelper::scale(10));
	ui->tableWidget->horizontalHeader()->setDefaultSectionSize(GUIHelper::scale(10));
	ui->tableWidget->verticalHeader()->setMinimumSectionSize(GUIHelper::scale(23));
	ui->tableWidget->verticalHeader()->setDefaultSectionSize(GUIHelper::scale(23));
	ui->tableWidget->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

	scene = new GraphicEQFilterGUIScene(ui->graphicsView);
	ui->graphicsView->setScene(scene);

	ResizeCorner* cornerWidget = new ResizeCorner(filterTable,
			QSize(0, ui->graphicsView->minimumHeight()),
			QSize(TABLE_RESIZE_ORIGIN - minimumTableWidth(), INT_MAX),
			[this]() {
		return QSize(TABLE_RESIZE_ORIGIN - ui->tableWidget->width(), ui->graphicsView->height());
	},
		[this](QSize size) {
		setTableWidth(TABLE_RESIZE_ORIGIN - size.width());
		setViewHeight(size.height());
		updatePreferredHeight();
	}, ui->graphicsView);
	ui->graphicsView->setCornerWidget(cornerWidget);

	ui->toolBar->addAction(ui->actionImport);
	ui->toolBar->addAction(ui->actionExport);
	QAction* exportFirAction = new QAction(ui->actionExport->icon(), tr("Export FIR"), this);
	ui->toolBar->addAction(exportFirAction);
	ui->toolBar->addAction(ui->actionInvertResponse);
	ui->toolBar->addAction(ui->actionNormalizeResponse);
	ui->toolBar->addAction(ui->actionResetResponse);
	connect(exportFirAction, &QAction::triggered, this, &GraphicEQFilterGUI::on_actionExportFIR_triggered);

	connect(scene, SIGNAL(nodeInserted(int,double,double)), this, SLOT(insertRow(int,double,double)));
	connect(scene, SIGNAL(nodeRemoved(int)), this, SLOT(removeRow(int)));
	connect(scene, SIGNAL(nodeUpdated(int,double,double)), this, SLOT(updateRow(int,double,double)));
	connect(scene, SIGNAL(nodeMoved(int,int)), this, SLOT(moveRow(int,int)));
	connect(scene, SIGNAL(nodeSelectionChanged(int,bool)), this, SLOT(selectRow(int,bool)));
	connect(scene, SIGNAL(updateModel()), this, SIGNAL(updateModel()));

	scene->setNodes(filter->getNodes());
	if (filterTable != nullptr && filterTable->getSelectedDevice() != nullptr && filterTable->getSelectedDevice()->getSampleRate() != 0)
	{
		deviceSampleRate = filterTable->getSelectedDevice()->getSampleRate();
		deviceGuid = QString::fromStdWString(filterTable->getSelectedDevice()->getDeviceGuid());
	}

	int bandCount = scene->verifyBands(filter->getNodes());
	switch (bandCount)
	{
	case 15:
		ui->radioButton15->click();
		break;
	case 31:
		ui->radioButton31->click();
		break;
	default:
		ui->radioButtonVar->click();
	}

	ui->tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	updatePreferredHeight();
}

GraphicEQFilterGUI::~GraphicEQFilterGUI()
{
	if (filterTable != nullptr)
		filterTable->removeEventFilter(this);
	delete ui;
}

void GraphicEQFilterGUI::changeEvent(QEvent* event)
{
	IFilterGUI::changeEvent(event);
	if (event->type() == QEvent::ApplicationPaletteChange ||
		event->type() == QEvent::PaletteChange ||
		event->type() == QEvent::ThemeChange)
	{
		updateThemeIcons();
	}
	if (event->type() == QEvent::FontChange ||
		event->type() == QEvent::ApplicationFontChange)
	{
		setTableWidth(ui->tableWidget->width());
		updatePreferredHeight();
	}
}

bool GraphicEQFilterGUI::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == filterTable &&
		(event->type() == QEvent::Resize || event->type() == QEvent::Show ||
			event->type() == QEvent::LayoutRequest))
	{
		// The row layout settles after the scroll viewport resizes. Re-clamp on
		// the next turn so a previously valid width cannot recreate an outer
		// horizontal scrollbar on a smaller window or monitor.
		QTimer::singleShot(0, this, [this]() {
			setTableWidth(ui->tableWidget->width());
			setViewHeight(ui->graphicsView->height());
		});
	}

	return IFilterGUI::eventFilter(watched, event);
}

int GraphicEQFilterGUI::minimumTableWidth() const
{
	// Do not derive this from minimumWidth(): setFixedWidth() updates that
	// property, which would otherwise make every wider value a new minimum.
	return GUIHelper::scale(96);
}

int GraphicEQFilterGUI::maximumTableWidth() const
{
	int availableWidth = width();
	if (filterTable != nullptr)
	{
		int viewportWidth = filterTable->width();
		if (QWidget* viewport = filterTable->parentWidget())
		{
			if (viewport->width() > 0)
				viewportWidth = viewportWidth > 0
					? (std::min)(viewportWidth, viewport->width())
					: viewport->width();
		}

		if (filterTable->isAncestorOf(this))
		{
			const int rowChromeWidth = mapTo(filterTable, QPoint(0, 0)).x();
			if (rowChromeWidth > 0 && rowChromeWidth < viewportWidth)
				viewportWidth -= rowChromeWidth;
		}

		if (viewportWidth > 0)
			availableWidth = viewportWidth;
	}

	if (QScreen* currentScreen = screen())
	{
		const int screenWidth = currentScreen->availableGeometry().width();
		availableWidth = availableWidth > 0
			? (std::min)(availableWidth, screenWidth)
			: screenWidth;
	}

	if (availableWidth <= 0)
		availableWidth = GUIHelper::scale(623);

	const int selectorWidth = ui->verticalLayout->sizeHint().width();
	const int graphWidth = GUIHelper::scale(160);
	const int spacing = (std::max)(0, ui->horizontalLayout->spacing()) * 2;
	const int edgeAllowance = GUIHelper::scale(12);
	return (std::max)(minimumTableWidth(),
		availableWidth - selectorWidth - graphWidth - spacing - edgeAllowance);
}

void GraphicEQFilterGUI::setTableWidth(int width)
{
	const int boundedWidth = qBound(minimumTableWidth(), width, maximumTableWidth());
	if (ui->tableWidget->width() != boundedWidth ||
		ui->tableWidget->minimumWidth() != boundedWidth ||
		ui->tableWidget->maximumWidth() != boundedWidth)
	{
		ui->tableWidget->setFixedWidth(boundedWidth);
	}
}

int GraphicEQFilterGUI::minimumViewHeight() const
{
	return GUIHelper::scale(100);
}

int GraphicEQFilterGUI::maximumViewHeight() const
{
	int availableHeight = height();
	if (filterTable != nullptr)
	{
		int viewportHeight = filterTable->height();
		if (QWidget* viewport = filterTable->parentWidget())
		{
			if (viewport->height() > 0)
				viewportHeight = viewportHeight > 0
					? (std::min)(viewportHeight, viewport->height())
					: viewport->height();
		}
		if (viewportHeight > 0)
			availableHeight = viewportHeight;
	}

	if (QScreen* currentScreen = screen())
	{
		const int screenHeight = currentScreen->availableGeometry().height();
		availableHeight = availableHeight > 0
			? (std::min)(availableHeight, screenHeight)
			: screenHeight;
	}

	if (availableHeight <= 0)
		availableHeight = GUIHelper::scale(480);

	const int surroundingRows = GUIHelper::scale(140);
	return (std::max)(minimumViewHeight(), availableHeight - surroundingRows);
}

void GraphicEQFilterGUI::setViewHeight(int height)
{
	const int boundedHeight = qBound(minimumViewHeight(), height, maximumViewHeight());
	if (ui->graphicsView->height() != boundedHeight ||
		ui->graphicsView->minimumHeight() != boundedHeight ||
		ui->graphicsView->maximumHeight() != boundedHeight)
	{
		ui->graphicsView->setFixedHeight(boundedHeight);
		updatePreferredHeight();
	}
}

void GraphicEQFilterGUI::updateThemeIcons()
{
	const QString iconPrefix = palette().color(QPalette::Window).lightnessF() < 0.5
		? QStringLiteral(":/icons/dark-mode/")
		: QStringLiteral(":/icons/");
	ui->actionInvertResponse->setIcon(QIcon(iconPrefix + QStringLiteral("invert_response.svg")));
	ui->actionNormalizeResponse->setIcon(QIcon(iconPrefix + QStringLiteral("normalize_response.svg")));
	ui->actionResetResponse->setIcon(QIcon(iconPrefix + QStringLiteral("reset_response.svg")));
}

void GraphicEQFilterGUI::store(QString& command, QString& parameters)
{
	command = "GraphicEQ";

	bool first = true;
	for (FilterNode node : scene->getNodes())
	{
		if (first)
			first = false;
		else
			parameters += "; ";

		parameters += QString("%1 %2").arg(node.freq).arg(node.dbGain);
	}
}

void GraphicEQFilterGUI::loadPreferences(const QVariantMap& prefs)
{
	bool validTableWidth = false;
	double storedTableWidth = prefs.value("tableWidth", DEFAULT_TABLE_WIDTH).toDouble(&validTableWidth);
	if (!validTableWidth || !std::isfinite(storedTableWidth))
		storedTableWidth = DEFAULT_TABLE_WIDTH;
	storedTableWidth = qBound(
		GUIHelper::invScale(minimumTableWidth()),
		storedTableWidth,
		GUIHelper::invScale(maximumTableWidth()));
	setTableWidth(GUIHelper::scale(storedTableWidth));
	bool validViewHeight = false;
	double storedViewHeight = prefs.value("viewHeight", DEFAULT_VIEW_HEIGHT).toDouble(&validViewHeight);
	if (!validViewHeight || !std::isfinite(storedViewHeight))
		storedViewHeight = DEFAULT_VIEW_HEIGHT;
	storedViewHeight = qBound(
		GUIHelper::invScale(minimumViewHeight()),
		storedViewHeight,
		GUIHelper::invScale(maximumViewHeight()));
	setViewHeight(GUIHelper::scale(storedViewHeight));
	updatePreferredHeight();
	double zoomX = GUIHelper::scaleZoom(prefs.value("zoomX", 1.0).toDouble());
	double zoomY = GUIHelper::scaleZoom(prefs.value("zoomY", 1.0).toDouble());
	scene->setZoom(zoomX, zoomY);
	bool ok;
	int scrollX = GUIHelper::scale(prefs.value("scrollX").toDouble(&ok));
	if (!ok)
		scrollX = round(scene->hzToX(20));
	int scrollY = GUIHelper::scale(prefs.value("scrollY").toDouble(&ok));
	if (!ok)
		scrollY = round(scene->dbToY(22));
	ui->graphicsView->setScrollOffsets(scrollX, scrollY);
}

void GraphicEQFilterGUI::storePreferences(QVariantMap& prefs)
{
	if (GUIHelper::invScale(ui->tableWidget->width()) != DEFAULT_TABLE_WIDTH)
		prefs.insert("tableWidth", GUIHelper::invScale(ui->tableWidget->width()));
	if (GUIHelper::invScale(ui->graphicsView->height()) != DEFAULT_VIEW_HEIGHT)
		prefs.insert("viewHeight", GUIHelper::invScale(ui->graphicsView->height()));
	if (GUIHelper::invScaleZoom(scene->getZoomX()) != 1.0)
		prefs.insert("zoomX", GUIHelper::invScaleZoom(scene->getZoomX()));
	if (GUIHelper::invScaleZoom(scene->getZoomY()) != 1.0)
		prefs.insert("zoomY", GUIHelper::invScaleZoom(scene->getZoomY()));
	QScrollBar* hScrollBar = ui->graphicsView->horizontalScrollBar();
	int value = hScrollBar->value();
	if (value != round(scene->hzToX(20)))
		prefs.insert("scrollX", GUIHelper::invScale(value));
	QScrollBar* vScrollBar = ui->graphicsView->verticalScrollBar();
	value = vScrollBar->value();
	if (value != round(scene->dbToY(22)))
		prefs.insert("scrollY", GUIHelper::invScale(value));
}

QSize GraphicEQFilterGUI::sizeHint() const
{
	QSize size = QWidget::sizeHint();
	size.setHeight(preferredHeight());
	return size;
}

QSize GraphicEQFilterGUI::minimumSizeHint() const
{
	QSize size = QWidget::minimumSizeHint();
	size.setHeight(preferredHeight());
	return size;
}

int GraphicEQFilterGUI::preferredHeight() const
{
	const int contentHeight = std::max(ui->graphicsView->height(), ui->graphicsView->minimumHeight())
		+ ui->toolBar->sizeHint().height()
		+ GUIHelper::scale(18);
	return qMax(contentHeight, layout()->totalSizeHint().height());
}

void GraphicEQFilterGUI::updatePreferredHeight()
{
	const int preferred = preferredHeight();
	setFixedHeight(preferred);
	updateGeometry();
}

void GraphicEQFilterGUI::insertRow(int index, double hz, double db)
{
	ui->tableWidget->blockSignals(true);
	ui->tableWidget->insertRow(index);
	QTableWidgetItem* freqItem = new QTableWidgetItem(QString("%1").arg(hz));
	ui->tableWidget->setItem(index, 0, freqItem);

	QTableWidgetItem* gainItem = new QTableWidgetItem(QString("%1").arg(db));
	ui->tableWidget->setItem(index, 1, gainItem);
	ui->tableWidget->blockSignals(false);
}

void GraphicEQFilterGUI::removeRow(int index)
{
	ui->tableWidget->blockSignals(true);
	ui->tableWidget->removeRow(index);
	ui->tableWidget->blockSignals(false);
}

void GraphicEQFilterGUI::updateRow(int index, double hz, double db)
{
	ui->tableWidget->blockSignals(true);
	QTableWidgetItem* freqItem = ui->tableWidget->item(index, 0);
	freqItem->setText(QString("%1").arg(hz));

	QTableWidgetItem* gainItem = ui->tableWidget->item(index, 1);
	gainItem->setText(QString("%1").arg(db));
	ui->tableWidget->blockSignals(false);
}

void GraphicEQFilterGUI::moveRow(int fromIndex, int toIndex)
{
	ui->tableWidget->blockSignals(true);
	QTableWidgetItem* freqItem = ui->tableWidget->takeItem(fromIndex, 0);
	QTableWidgetItem* gainItem = ui->tableWidget->takeItem(fromIndex, 1);
	bool selected = ui->tableWidget->selectionModel()->isRowSelected(fromIndex, ui->tableWidget->rootIndex());
	ui->tableWidget->removeRow(fromIndex);
	ui->tableWidget->insertRow(toIndex);
	ui->tableWidget->setItem(toIndex, 0, freqItem);
	ui->tableWidget->setItem(toIndex, 1, gainItem);
	if (selected)
	{
		QModelIndex modelIndex = ui->tableWidget->model()->index(toIndex, 0);
		ui->tableWidget->selectionModel()->select(modelIndex, QItemSelectionModel::Rows | QItemSelectionModel::Select);
		ui->tableWidget->scrollTo(modelIndex);
	}
	ui->tableWidget->blockSignals(false);
}

void GraphicEQFilterGUI::selectRow(int index, bool select)
{
	ui->tableWidget->blockSignals(true);
	QItemSelectionModel::SelectionFlags command = QItemSelectionModel::Rows;
	if (select)
		command |= QItemSelectionModel::Select;
	else
		command |= QItemSelectionModel::Deselect;
	QModelIndex modelIndex = ui->tableWidget->model()->index(index, 0);
	ui->tableWidget->selectionModel()->select(modelIndex, command);
	ui->tableWidget->scrollTo(modelIndex);
	ui->tableWidget->blockSignals(false);
}

void GraphicEQFilterGUI::on_tableWidget_cellChanged(int row, int column)
{
	if (column == 0)
	{
		QTableWidgetItem* freqItem = ui->tableWidget->item(row, 0);
		bool ok;
		double freq = freqItem->text().toDouble(&ok);
		if (ok)
		{
			double gain = scene->getNodes()[row].dbGain;
			scene->setNode(row, freq, gain);
		}
		else
		{
			freq = scene->getNodes()[row].freq;
			freqItem->setText(QString("%1").arg(freq));
		}
	}
	else if (column == 1)
	{
		QTableWidgetItem* gainItem = ui->tableWidget->item(row, 1);
		bool ok;
		double gain = gainItem->text().toDouble(&ok);
		if (ok)
		{
			double freq = scene->getNodes()[row].freq;
			scene->setNode(row, freq, gain);
		}
		else
		{
			gain = scene->getNodes()[row].dbGain;
			gainItem->setText(QString("%1").arg(gain));
		}
	}
}

void GraphicEQFilterGUI::on_tableWidget_itemSelectionChanged()
{
	QSet<int> selectedRows;
	for (QTableWidgetItem* item : ui->tableWidget->selectedItems())
		selectedRows.insert(item->row());

	scene->setSelectedNodes(selectedRows);
}

void GraphicEQFilterGUI::on_radioButton15_toggled(bool checked)
{
	if (checked)
	{
		scene->setBandCount(15);
		setFreqEditable(false);
	}
}

void GraphicEQFilterGUI::on_radioButton31_toggled(bool checked)
{
	if (checked)
	{
		scene->setBandCount(31);
		setFreqEditable(false);
	}
}

void GraphicEQFilterGUI::on_radioButtonVar_toggled(bool checked)
{
	if (checked)
	{
		scene->setBandCount(-1);
		setFreqEditable(true);
	}
}

void GraphicEQFilterGUI::setFreqEditable(bool editable)
{
	ui->tableWidget->blockSignals(true);
	for (int i = 0; i < ui->tableWidget->rowCount(); i++)
	{
		QTableWidgetItem* item = ui->tableWidget->item(i, 0);
		if (editable)
			item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsEnabled);
		else
			item->setFlags(item->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsEnabled));
	}
	ui->tableWidget->blockSignals(false);
}

unsigned GraphicEQFilterGUI::currentDeviceSampleRate() const
{
	if (!deviceGuid.isEmpty())
	{
		DeviceAPOInfo info;
		if (info.load(deviceGuid.toStdWString()) && info.getSampleRate() != 0)
			return info.getSampleRate();
	}

	return deviceSampleRate != 0 ? deviceSampleRate : 48000;
}

void GraphicEQFilterGUI::on_actionImport_triggered()
{
	QFileInfo fileInfo(configPath);
	QFileDialog dialog(this, tr("Import frequency response"), fileInfo.absolutePath(), "*.csv");
	dialog.setFileMode(QFileDialog::ExistingFiles);
	QStringList nameFilters;
	nameFilters.append(tr("Frequency response (*.csv)"));
	nameFilters.append(tr("All files (*.*)"));
	dialog.setNameFilters(nameFilters);
	if (dialog.exec() == QDialog::Accepted)
	{
		vector<FilterNode> newNodes;

		for (QString path : dialog.selectedFiles())
		{
			QFile file(path);
			if (file.open(QFile::ReadOnly))
			{
				QTextStream stream(&file);
				while (!stream.atEnd())
				{
					QString text = stream.readLine();
					if (text.startsWith('*'))
						continue;

					if (!text.contains('.'))
						text = text.replace(',', '.');
					QRegularExpressionMatchIterator it = numberRegEx.globalMatch(text);
					while (it.hasNext())
					{
						QRegularExpressionMatch match = it.next();
						QString freqString = match.captured();
						bool ok;
						double freq = freqString.toDouble(&ok);
						if (ok && it.hasNext())
						{
							match = it.next();
							QString gainString = match.captured();
							double gain = gainString.toDouble(&ok);
							if (ok)
								newNodes.push_back(FilterNode(freq, gain));
						}
					}
				}
			}
		}
		sort(newNodes.begin(), newNodes.end());

		scene->setNodes(newNodes);

		int bandCount = scene->verifyBands(newNodes);
		if (bandCount != scene->getBandCount())
		{
			switch (bandCount)
			{
			case 15:
				ui->radioButton15->click();
				break;
			case 31:
				ui->radioButton31->click();
				break;
			default:
				ui->radioButtonVar->click();
			}
		}
		else
		{
			emit updateModel();
		}
	}
}

void GraphicEQFilterGUI::on_actionExport_triggered()
{
	QFileInfo fileInfo(configPath);
	QFileDialog dialog(this, tr("Export frequency response"), fileInfo.absolutePath(), "*.csv");
	dialog.setFileMode(QFileDialog::AnyFile);
	dialog.setAcceptMode(QFileDialog::AcceptSave);
	QStringList nameFilters;
	nameFilters.append(tr("Frequency response (*.csv)"));
	nameFilters.append(tr("All files (*.*)"));
	dialog.setNameFilters(nameFilters);
	dialog.setDefaultSuffix(".csv");

	if (dialog.exec() == QDialog::Accepted)
	{
		QString savePath = dialog.selectedFiles().first();

		QFile file(savePath);
		if (file.open(QFile::WriteOnly | QFile::Truncate))
		{
			QTextStream stream(&file);
			for (FilterNode node : scene->getNodes())
			{
				stream << node.freq << '\t' << node.dbGain << '\n';
			}
			stream.flush();
		}
	}
}

void GraphicEQFilterGUI::on_actionExportFIR_triggered()
{
	QFileInfo fileInfo(configPath);
	const unsigned sampleRate = currentDeviceSampleRate();
	QFileDialog dialog(this, tr("Export FIR impulse response"), fileInfo.absolutePath(), "*.wav");
	dialog.setFileMode(QFileDialog::AnyFile);
	dialog.setAcceptMode(QFileDialog::AcceptSave);
	QStringList nameFilters;
	nameFilters.append(tr("FIR impulse response (*.wav)"));
	nameFilters.append(tr("All files (*.*)"));
	dialog.setNameFilters(nameFilters);
	dialog.setDefaultSuffix(".wav");
	dialog.selectFile(QString("GraphicEQ_FIR_%1Hz.wav").arg(sampleRate));

	if (dialog.exec() != QDialog::Accepted)
		return;

	QString savePath = dialog.selectedFiles().first();
	std::vector<double> impulse = GraphicEQFilter::createImpulseResponse(scene->getNodes(), 16384, static_cast<float>(sampleRate));
	if (impulse.empty())
	{
		QMessageBox::warning(this, tr("Export FIR"), tr("Could not generate FIR impulse response."));
		return;
	}

	SF_INFO info = {};
	info.channels = 1;
	info.samplerate = static_cast<int>(sampleRate);
	info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
	SNDFILE* file = sf_wchar_open(savePath.toStdWString().c_str(), SFM_WRITE, &info);
	if (file == nullptr)
	{
		QMessageBox::warning(this, tr("Export FIR"), tr("Could not create FIR file."));
		return;
	}

	sf_count_t written = sf_writef_double(file, impulse.data(), static_cast<sf_count_t>(impulse.size()));
	sf_close(file);
	if (written != static_cast<sf_count_t>(impulse.size()))
	{
		QMessageBox::warning(this, tr("Export FIR"), tr("Could not write the complete FIR file."));
		return;
	}

	QMessageBox::information(this, tr("Export FIR"), tr("Exported FIR at %1 Hz.").arg(sampleRate));
}

void GraphicEQFilterGUI::on_actionInvertResponse_triggered()
{
	vector<FilterNode> newNodes = scene->getNodes();
	for (FilterNode& node : newNodes)
		node.dbGain = -node.dbGain;

	scene->setNodes(newNodes);
	emit updateModel();
}

void GraphicEQFilterGUI::on_actionNormalizeResponse_triggered()
{
	vector<FilterNode> newNodes = scene->getNodes();

	double maxGain = -DBL_MAX;
	for (FilterNode node : newNodes)
	{
		if (node.dbGain > maxGain)
			maxGain = node.dbGain;
	}

	if (maxGain != 0)
	{
		for (FilterNode& node : newNodes)
			node.dbGain -= maxGain;

		scene->setNodes(newNodes);
		emit updateModel();
	}
}

void GraphicEQFilterGUI::on_actionResetResponse_triggered()
{
	vector<FilterNode> newNodes = scene->getNodes();
	QSet<int> selectedIndices = scene->getSelectedIndices();

	int index = 0;
	for (FilterNode& node : newNodes)
	{
		if (selectedIndices.isEmpty() || selectedIndices.contains(index))
			node.dbGain = 0.0;
		index++;
	}

	scene->setNodes(newNodes);
	scene->setSelectedNodes(selectedIndices);
	emit updateModel();
}
