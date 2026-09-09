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

#include <QToolBar>
#include <QApplication>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOption>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QVBoxLayout>
#include <QMap>
#include <algorithm>

#include "Editor/helpers/GUIHelper.h"
#include "FilterTableRow.h"
#include "ui_FilterTableRow.h"

namespace
{
	void detachFromLayout(QLayout* layout, QWidget* widget)
	{
		layout->removeWidget(widget);
		for (int i = 0; i < layout->count(); ++i)
			if (QLayout* child = layout->itemAt(i)->layout())
				detachFromLayout(child, widget);
	}

	void normalizeModuleHeader(IFilterGUI* gui)
	{
		if (gui->property("studioHeaderNormalized").toBool()) return;
		gui->setProperty("studioHeaderNormalized", true);
		for (auto* child : gui->findChildren<IFilterGUI*>())
			normalizeModuleHeader(child);
		// Explicit identities avoid mistaking parameter labels for module titles.
		const QMap<QString, QString> titles = {
			{"PreampFilterGUI", "label"}, {"DelayFilterGUI", "delayLabel"},
			{"ChannelFilterGUI", "label"}, {"DeviceFilterGUI", "label"},
			{"StageFilterGUI", "label"}, {"GraphicEQFilterGUI", "label"},
			{"IncludeFilterGUI", "includeLabel"}, {"CopyFilterGUI", "copyLabel"},
			{"ConvolutionFilterGUI", "convolutionLabel"}, {"VSTPluginFilterGUI", "label"},
			{"LoudnessCorrectionFilterGUI", "label"}, {"BiQuadFilterGUI", "typeComboBox"}
		};
		const QString name = titles.value(QString::fromLatin1(gui->metaObject()->className()));
		if (!gui->layout()) return;
		QWidget* title = name.isEmpty() ? nullptr : gui->findChild<QWidget*>(name);
		const QMap<QString, QPair<const char*, const char*>> generatedTitles = {
			{"PanFilterGUI", {"PanFilterGUIFactory", QT_TRANSLATE_NOOP("PanFilterGUIFactory", "Pan")}},
			{"CrossfeedFilterGUI", {"CrossfeedFilterGUIFactory", QT_TRANSLATE_NOOP("CrossfeedFilterGUIFactory", "Crossfeed")}},
			{"ChorusFilterGUI", {"ChorusFilterGUIFactory", QT_TRANSLATE_NOOP("ChorusFilterGUIFactory", "Chorus")}},
			{"ReverbFilterGUI", {"ReverbFilterGUIFactory", QT_TRANSLATE_NOOP("ReverbFilterGUIFactory", "Reverb")}},
			{"ToneGeneratorFilterGUI", {"ToneGeneratorFilterGUIFactory", QT_TRANSLATE_NOOP("ToneGeneratorFilterGUIFactory", "Tone generator")}},
			{"VUMeterFilterGUI", {"VUMeterFilterGUIFactory", QT_TRANSLATE_NOOP("VUMeterFilterGUIFactory", "VU meter")}},
			{"ParametricEQFilterGUI", {"ParametricEQFilterGUIFactory", QT_TRANSLATE_NOOP("ParametricEQFilterGUIFactory", "Parametric EQ")}}
		};
		if (!title && generatedTitles.contains(gui->objectName()))
		{
			const auto text = generatedTitles.value(gui->objectName());
			title = new QLabel(QCoreApplication::translate(text.first, text.second), gui);
			title->setObjectName(QStringLiteral("studioGeneratedTitle"));
		}
		if (!title) return;
		detachFromLayout(gui->layout(), title);
		auto* body = new QWidget(gui);
		body->setObjectName(QStringLiteral("studioModuleBody"));
		body->setLayout(gui->layout());
		auto* root = new QVBoxLayout(gui);
		root->setContentsMargins(0, 0, 0, 0);
		root->setSpacing(GUIHelper::scale(5));
		title->setProperty("studioModuleTitle", true);
		title->setMinimumHeight(GUIHelper::scale(28));
		title->setSizePolicy(title->sizePolicy().horizontalPolicy(), QSizePolicy::Fixed);
		if (auto* label = qobject_cast<QLabel*>(title))
		{
			label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
			QFont font = label->font();
			font.setWeight(QFont::DemiBold);
			label->setFont(font);
		}
		root->addWidget(title, 0, Qt::AlignLeft);
		root->addWidget(body);
	}

	class ElidingCommandLabel final : public QLabel
	{
	public:
		explicit ElidingCommandLabel(const QString& command, QWidget* parent = nullptr)
			: QLabel(command, parent)
		{
			setObjectName(QStringLiteral("elidingCommandLabel"));
			setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
			setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			setMinimumWidth(0);
			setToolTip(command);
			setAccessibleName(command);
		}

		QSize sizeHint() const override
		{
			QSize size = QLabel::sizeHint();
			const int preferredWidth = fontMetrics().horizontalAdvance(QLatin1Char('M')) * 48;
			size.setWidth((std::min)(size.width(), preferredWidth));
			return size;
		}

		QSize minimumSizeHint() const override
		{
			QSize size = QLabel::minimumSizeHint();
			size.setWidth(0);
			return size;
		}

	protected:
		void paintEvent(QPaintEvent*) override
		{
			QPainter painter(this);
			QStyleOption option;
			option.initFrom(this);
			style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

			QRect textRect = rect().marginsRemoved(contentsMargins());
			const int labelMargin = margin();
			textRect.adjust(labelMargin, labelMargin, -labelMargin, -labelMargin);
			const QString displayText = fontMetrics().elidedText(
				text(), Qt::ElideMiddle, (std::max)(0, textRect.width()));
			style()->drawItemText(
				&painter,
				textRect,
				alignment(),
				palette(),
				isEnabled(),
				displayText,
				foregroundRole());
		}
	};

	void appendFilterRowDebugLog(const QString& message)
	{
		QFile file(QDir::temp().absoluteFilePath("EqApoOutProcHost-debug.log"));
		if (file.open(QIODevice::Append | QIODevice::Text))
		{
			QTextStream stream(&file);
			stream << "[row] " << message << "\n";
		}
	}
}

FilterTableRow::FilterTableRow(FilterTable* table, int number, FilterTable::Item* item, IFilterGUI* gui)
	: QWidget(table),
	ui(new Ui::FilterTableRow)
{
	ui->setupUi(this);
	ui->actionAdd->setIcon(GUIHelper::createAccentAddIcon());
	ui->actionCloneAbove->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Duplicate));
	ui->actionCloneBelow->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Duplicate));
	ui->actionRemove->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Remove));
	ui->actionEditText->setIcon(GUIHelper::createThemeIcon(GUIHelper::ThemeIcon::Edit));
	setAttribute(Qt::WA_StyledBackground, false);
	ui->labelNumber->setMinimumWidth(GUIHelper::scale(38));
	ui->labelNumber->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
	ui->labelNumber->setContentsMargins(0, GUIHelper::scale(6), 0, 0);
	ui->horizontalLayout->setContentsMargins(
		0,
		GUIHelper::scale(6),
		GUIHelper::scale(6),
		GUIHelper::scale(4));
	QFont numberFont = font();
	numberFont.setWeight(QFont::DemiBold);
	ui->labelNumber->setFont(numberFont);

	this->table = table;
	this->item = item;
	this->gui = gui;

	ui->labelNumber->setText(QString("%1").arg(number));

	ui->toolBar->addAction(ui->actionAdd);
	ui->toolBar->addAction(ui->actionCloneAbove);
	ui->toolBar->addAction(ui->actionCloneBelow);
	ui->toolBar->addAction(ui->actionRemove);
	ui->toolBar->addAction(ui->actionEditText);
	ui->toolBar->setOrientation(Qt::Horizontal);
	ui->toolBar->updateMaximumHeight();
	ui->horizontalLayout->setAlignment(ui->toolBar, Qt::AlignTop);

	ui->stackedWidget->setContentsMargins(
		GUIHelper::scale(3),
		GUIHelper::scale(2),
		GUIHelper::scale(2),
		GUIHelper::scale(2));

	if (gui != NULL)
	{
		normalizeModuleHeader(gui);
		connect(gui, SIGNAL(updateModel()), this, SLOT(updateModel()));
		ui->stackedWidget->addWidget(gui);
	}
	else
	{
		ui->stackedWidget->addWidget(new ElidingCommandLabel(item->text, ui->stackedWidget));
	}
	ui->stackedWidget->setCurrentIndex(1);
}

FilterTableRow::~FilterTableRow()
{
	delete ui;
}

QRect FilterTableRow::getHeaderRect()
{
	QRect rect(0, 0, ui->labelNumber->geometry().right() + 1, height());

	return rect;
}

void FilterTableRow::editText()
{
	if (!ui->actionEditText->isChecked())
		ui->actionEditText->trigger();
}

QSize FilterTableRow::sizeHint() const
{
	QSize size = QWidget::sizeHint().expandedTo(minimumSizeHint());
	// The grid owns the available row width. Do not let a plug-in panel's
	// preferred width force the outer editor back to horizontal scrolling.
	size.setWidth(0);
	return size;
}

QSize FilterTableRow::minimumSizeHint() const
{
	QSize size = QWidget::minimumSizeHint();
	// Rows must remain viewport-compressible. Wide plug-in names and the
	// experimental tool panels may have a large preferred width, but must not
	// recreate the outer horizontal scrollbar that the modern UI removed.
	size.setWidth(0);
	QWidget* current = ui->stackedWidget->currentWidget();
	if (current != nullptr)
	{
		QSize childSize = current->minimumSizeHint().expandedTo(current->sizeHint()).expandedTo(current->minimumSize());
		QMargins margins = ui->horizontalLayout->contentsMargins() + ui->stackedWidget->contentsMargins();
		childSize.rheight() += margins.top() + margins.bottom() + GUIHelper::scale(4);
		size.setHeight(std::max(size.height(), std::max(childSize.height(), ui->toolBar->maximumHeight() + GUIHelper::scale(4))));
	}
	return size;
}

void FilterTableRow::mouseDoubleClickEvent(QMouseEvent*)
{
	if (gui == NULL && ui->stackedWidget->currentIndex() == 1)
		ui->actionEditText->trigger();
}

void FilterTableRow::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);

	const qreal inset = GUIHelper::scale(1.0);
	const QRectF card = QRectF(rect()).adjusted(
		inset,
		inset,
		-inset,
		-inset);
	const bool selected = table->getSelectedItems().contains(item);
	const bool focused = table->getFocusedItem() == item;
	const QPalette rowPalette = palette();
	const QColor surface = rowPalette.color(QPalette::Base);
	QColor accent = rowPalette.color(QPalette::Highlight);
	QColor border = rowPalette.color(QPalette::Mid);
	const bool highContrast = qApp
		&& qApp->property("eqapoModernThemeHighContrast").toBool();

	painter.setPen(Qt::NoPen);
	painter.setBrush(surface);
	painter.drawRoundedRect(card, 9, 9);

	if ((selected || focused) && !highContrast)
	{
		QColor overlay = selected ? accent : rowPalette.color(QPalette::AlternateBase);
		overlay.setAlpha(selected ? 38 : 90);
		painter.setBrush(overlay);
		painter.drawRoundedRect(card, 9, 9);
	}

	QPainterPath clip;
	clip.addRoundedRect(card, 9, 9);
	painter.save();
	painter.setClipPath(clip);
	QRectF rail = card;
	rail.setRight(ui->labelNumber->geometry().right() + GUIHelper::scale(3));
	if (!highContrast)
	{
		QColor railColor = accent;
		railColor.setAlpha(selected ? 62 : 12);
		painter.fillRect(rail, railColor);
	}
	QColor railEdge = accent;
	if (!highContrast)
		railEdge.setAlpha(selected ? 220 : 90);
	painter.setPen(QPen(railEdge, GUIHelper::scale(selected ? 2.0 : 1.0)));
	painter.drawLine(
		QPointF(rail.right(), rail.top()),
		QPointF(rail.right(), rail.bottom()));
	painter.restore();

	if (selected)
		border = accent;
	else if (focused)
	{
		border = rowPalette.color(QPalette::WindowText);
		if (!highContrast)
			border.setAlpha(105);
	}
	painter.setBrush(Qt::NoBrush);
	painter.setPen(QPen(
		border,
		GUIHelper::scale(selected && focused ? 2.0 : 1.0)));
	painter.drawRoundedRect(card, 9, 9);
}

void FilterTableRow::updateModel()
{
	IFilterGUI* sender = qobject_cast<IFilterGUI*>(QObject::sender());
	QString command;
	QString parameters;
	sender->store(command, parameters);

	item->text = command + ": " + parameters;

	qDebug("Updated item text to %s", item->text.toStdString().c_str());
}

void FilterTableRow::on_actionAdd_triggered()
{
	QMenu* menu = table->createAddPopupMenu();
	QRect rect = ui->toolBar->actionGeometry(ui->actionAdd);
	QPoint p = ui->toolBar->mapToGlobal(QPoint(rect.x(), rect.y() + rect.height()));
	QAction* action = menu->exec(p);
	ui->actionAdd->setChecked(false);
	if (action != NULL)
	{
		FilterTemplate t = action->data().value<FilterTemplate>();
		QString line = t.getLine();
		table->addLine(line, item);
		table->updateGuis();
	}
	delete menu;
}

void FilterTableRow::on_actionCloneAbove_triggered()
{
	table->cloneItem(item, false);
	table->updateGuis();
}

void FilterTableRow::on_actionCloneBelow_triggered()
{
	table->cloneItem(item, true);
	table->updateGuis();
}

void FilterTableRow::on_actionRemove_triggered()
{
	appendFilterRowDebugLog("remove clicked itemText=" + item->text + " gui=" + QString(gui != NULL ? "true" : "false"));
	if (table->removeItem(item))
		table->updateGuis();
}

void FilterTableRow::on_actionEditText_triggered(bool checked)
{
	if (checked)
	{
		if (!lastEditTime.isValid() || lastEditTime.msecsTo(QDateTime::currentDateTimeUtc()) > 100)
		{
			// Recover state before copying the serialized row into the editor.
			// Otherwise a successful late preflight could be overwritten by text
			// that was captured while the external VST panel was still changing.
			if (!table->prepareItemReplacement(item))
			{
				ui->actionEditText->setChecked(false);
				return;
			}
			ui->lineEdit->setText(item->text);
			ui->stackedWidget->setCurrentIndex(0);
			ui->lineEdit->setFocus();
		}
		else
		{
			ui->actionEditText->setChecked(false);
		}
	}
}

void FilterTableRow::on_lineEdit_editingFinished()
{
	if (ui->stackedWidget->currentIndex() == 0 && !editingDone)
	{
		if (ui->lineEdit->text() != item->text)
		{
			editingDone = true;
			if (table->replaceItemText(item, ui->lineEdit->text()))
			{
				table->updateModel();
				// set focus to table so that enter key does not cause scrolling down
				table->setFocus();
				table->updateGuis();
			}
			else
			{
				// The row owns external state that could not be stopped. Restore
				// the committed text and leave the existing GUI/list state intact.
				ui->lineEdit->setText(item->text);
				ui->stackedWidget->setCurrentIndex(1);
				ui->actionEditText->setChecked(false);
				table->setFocus();
				editingDone = false;
			}
		}
		else
		{
			ui->stackedWidget->setCurrentIndex(1);
			lastEditTime = QDateTime::currentDateTimeUtc();
			ui->actionEditText->setChecked(false);
		}
	}
}

void FilterTableRow::on_lineEdit_editingCanceled()
{
	if (ui->stackedWidget->currentIndex() == 0)
	{
		// will cause editingFinished to be called, so prevent committing
		editingDone = true;
		table->setFocus();
		ui->stackedWidget->setCurrentIndex(1);
		editingDone = false;
		ui->actionEditText->setChecked(false);
	}
}
