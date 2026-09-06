#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QVector>

#include "Editor/IFilterGUIFactory.h"

class VUMeterPanel;
class VUMeterStatsPanel;

class AudioToolFilterGUI : public IFilterGUI
{
	Q_OBJECT

public:
	explicit AudioToolFilterGUI(const QString& command, const QString& parameters);
	~AudioToolFilterGUI() override;
	void store(QString& command, QString& parameters) override;
	bool commitDelete() override;

private:
	QWidget* addSliderControl(QGridLayout* grid, const QString& label, QDoubleSpinBox** spin, double min, double max, double value, const QString& suffix, int row, int column, int decimals = 1);
	QPushButton* addModuleResetButton(QGridLayout* grid, int row, int column, int columnSpan = 1);
	void addChannelSelector(QGridLayout* grid, const QString& parameters, int row, int column, int columnSpan, const QString& selectorOverride = QString());
	QString selectedChannels() const;
	void resetModuleToDefaults();
	void updateMeterPanel();
	void destroyMeterDialog();

	QString commandName;
	QString meterId = "default";
	QString originalChannelSelector = "all";
	QStringList preservedChannelTokens;
	bool channelSelectionEdited = false;
	QPushButton* stateButton = nullptr;
	QComboBox* typeComboBox = nullptr;
	QDoubleSpinBox* frequencySpinBox = nullptr;
	QDoubleSpinBox* startSpinBox = nullptr;
	QDoubleSpinBox* endSpinBox = nullptr;
	QDoubleSpinBox* durationSpinBox = nullptr;
	QDoubleSpinBox* levelSpinBox = nullptr;
	QComboBox* modeComboBox = nullptr;
	QDoubleSpinBox* positionSpinBox = nullptr;
	QDoubleSpinBox* widthSpinBox = nullptr;
	QDoubleSpinBox* amountSpinBox = nullptr;
	QComboBox* crossfeedAlgorithmComboBox = nullptr;
	QComboBox* crossfeedPresetComboBox = nullptr;
	QComboBox* rmsStandardComboBox = nullptr;
	QComboBox* lufsStandardComboBox = nullptr;
	QDoubleSpinBox* headCircumferenceSpinBox = nullptr;
	QDoubleSpinBox* headWidthSpinBox = nullptr;
	QDoubleSpinBox* headLengthSpinBox = nullptr;
	QDoubleSpinBox* angleSpinBox = nullptr;
	QDoubleSpinBox* cutoffSpinBox = nullptr;
	QDoubleSpinBox* directSpinBox = nullptr;
	QDoubleSpinBox* rateSpinBox = nullptr;
	QDoubleSpinBox* depthSpinBox = nullptr;
	QDoubleSpinBox* mixSpinBox = nullptr;
	QDoubleSpinBox* feedbackSpinBox = nullptr;
	QDoubleSpinBox* roomSpinBox = nullptr;
	QDoubleSpinBox* dampingSpinBox = nullptr;
	QDoubleSpinBox* wetSpinBox = nullptr;
	QDoubleSpinBox* drySpinBox = nullptr;
	QPushButton* panelButton = nullptr;
	QPushButton* resetButton = nullptr;
	QFrame* panelFrame = nullptr;
	VUMeterPanel* meterPanel = nullptr;
	VUMeterStatsPanel* meterStatsPanel = nullptr;
	QDialog* meterDialog = nullptr;
	QVector<QCheckBox*> channelChecks;
};

class ToneGeneratorFilterGUIFactory : public IFilterGUIFactory
{
	Q_OBJECT
public:
	QList<FilterTemplate> createFilterTemplates() override;
	IFilterGUI* createFilterGUI(QString& command, QString& parameters) override;
};

class PanFilterGUIFactory : public IFilterGUIFactory
{
	Q_OBJECT
public:
	QList<FilterTemplate> createFilterTemplates() override;
	IFilterGUI* createFilterGUI(QString& command, QString& parameters) override;
};

class CrossfeedFilterGUIFactory : public IFilterGUIFactory
{
	Q_OBJECT
public:
	QList<FilterTemplate> createFilterTemplates() override;
	IFilterGUI* createFilterGUI(QString& command, QString& parameters) override;
};

class ChorusFilterGUIFactory : public IFilterGUIFactory
{
	Q_OBJECT
public:
	QList<FilterTemplate> createFilterTemplates() override;
	IFilterGUI* createFilterGUI(QString& command, QString& parameters) override;
};

class ReverbFilterGUIFactory : public IFilterGUIFactory
{
	Q_OBJECT
public:
	QList<FilterTemplate> createFilterTemplates() override;
	IFilterGUI* createFilterGUI(QString& command, QString& parameters) override;
};

class VUMeterFilterGUIFactory : public IFilterGUIFactory
{
	Q_OBJECT
public:
	QList<FilterTemplate> createFilterTemplates() override;
	IFilterGUI* createFilterGUI(QString& command, QString& parameters) override;
};
