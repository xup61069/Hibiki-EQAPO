#include <QTest>
#include <QDoubleSpinBox>
#include "Editor/helpers/VolumeTakeoverManager.h"
#include "filters/loudnessCorrection/HibikiVolumeTakeoverShared.h"

extern EndpointVolumeState testEndpoint;
extern bool testEndpointWriteFails;

class VolumeTakeoverTests : public QObject
{
	Q_OBJECT
	VolumeTakeoverManager* manager = nullptr;
	HibikiVolumeTakeoverSharedData shared = {};
	void hook(UINT key, DWORD time)
	{
		KBDLLHOOKSTRUCT input = {};
		input.vkCode = key;
		input.time = time;
		QCOMPARE(manager->LowLevelKeyboardProc(HC_ACTION, WM_KEYDOWN, reinterpret_cast<LPARAM>(&input)), LRESULT(1));
	}
	void hotkey(int keyIndex, DWORD time)
	{
		MSG input = {};
		input.message = WM_HOTKEY;
		input.wParam = 0x6B10 + keyIndex;
		input.time = time;
		QVERIFY(manager->nativeEventFilter("windows_dispatcher_MSG", &input, nullptr));
	}
	void drain() { QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); }
private slots:
	void init()
	{
		testEndpoint = {};
		testEndpointWriteFails = false;
		shared = {};
		manager = new VolumeTakeoverManager;
		manager->pollTimer.stop();
		manager->osdEnabled = false;
		manager->takeoverEnabled = true;
		manager->takeoverScalar = 0.5;
		manager->takeoverLevelDb = -50.0;
		manager->takeoverShared = &shared;
		manager->nextHookRearmTick = GetTickCount64() + 100000;
		// A sentinel prevents the watchdog from installing a real hook.
		manager->s_keyboardHook = reinterpret_cast<HHOOK>(1);
		for (bool& value : manager->mediaHotkeyRegistered) value = true;
	}
	void cleanup()
	{
		manager->takeoverEnabled = false;
		manager->takeoverShared = nullptr;
		manager->s_keyboardHook = nullptr;
		for (bool& value : manager->mediaHotkeyRegistered) value = false;
		delete manager;
		manager = nullptr;
	}
	void delayedDuplicateCountsOnce()
	{
		hook(VK_VOLUME_UP, 1000);
		Sleep(150); // Model a busy UI thread while both notifications wait.
		hotkey(0, 1000);
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.52);
	}
	void hotkeyBeforeHookCountsOnce()
	{
		hotkey(0, 2000);
		hook(VK_VOLUME_UP, 2000);
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.52);
	}
	void queuedRepeatPreservesEveryPress()
	{
		for (DWORD time = 3000; time < 3020; ++time) hook(VK_VOLUME_UP, time);
		Sleep(150);
		for (DWORD time = 3000; time < 3020; ++time) hotkey(0, time);
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.9);
	}
	void mutePreservesManualPositionThroughUiFeedback()
	{
		manager->setManualMode(true, -50.0);
		QDoubleSpinBox spin;
		spin.setRange(-100.0, 0.0);
		spin.setValue(-50.0);
		connect(&spin, qOverload<double>(&QDoubleSpinBox::valueChanged), manager, &VolumeTakeoverManager::setManualVolumeDb);
		connect(manager, &VolumeTakeoverManager::volumeChangedExternal, &spin,
			[&spin](double level, double, bool) { spin.setValue(level); });
		manager->toggleMute();
		QCOMPARE(manager->getManualVolumeDb(), -50.0);
		manager->toggleMute();
		QCOMPARE(manager->getTakeoverScalar(), 0.5);
	}
	void defaultStepIsConsistentInManualMode()
	{
		manager->setManualMode(true, -50.0);
		manager->stepVolume(true);
		QCOMPARE(manager->getTakeoverScalar(), 0.52);
	}
	void configuredSteps_data()
	{
		QTest::addColumn<int>("percent");
		QTest::addColumn<bool>("manual");
		for (int step : {1, 2, 3, 4, 5, 10, 100})
			for (bool mode : {false, true})
				QTest::newRow(qPrintable(QString("%1-%2").arg(step).arg(mode))) << step << mode;
	}
	void configuredSteps()
	{
		QFETCH(int, percent);
		QFETCH(bool, manual);
		manager->setVolumeStepPercent(percent);
		if (manual) manager->setManualMode(true, -50.0);
		manager->stepVolume(true);
		QCOMPARE(manager->getTakeoverScalar(), (std::min)(100, 50 + percent) / 100.0);
		manager->setVolumeScalar(0.5);
		manager->stepVolume(false);
		QCOMPARE(manager->getTakeoverScalar(), (std::max)(0, 50 - percent) / 100.0);
	}
	void invalidSavedStepUsesDefault()
	{
		for (int step : {-4, 0, 101, 2147483647})
		{
			manager->setVolumeStepPercent(step);
			QCOMPARE(manager->getVolumeStepPercent(), 2);
		}
	}
	void timestampZeroAndWrapDoNotDebounceRealKeys()
	{
		for (DWORD time : {0xfffffffeu, 0xffffffffu, 0u, 1u})
		{
			hook(VK_VOLUME_UP, time);
			// Exercise matching directly so the activation-time guard is separate.
			manager->queueMediaKey(0, time, MediaKeyDeduplicator::Source::Hotkey);
		}
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.58);
	}
	void sameTickOccurrencesStillCountSeparately()
	{
		hook(VK_VOLUME_UP, 4000);
		hook(VK_VOLUME_UP, 4000);
		hotkey(0, 4000);
		hotkey(0, 4000);
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.54);
	}
	void muteDuplicateTogglesOnce()
	{
		hook(VK_VOLUME_MUTE, 5000);
		Sleep(150);
		hotkey(2, 5000);
		drain();
		QVERIFY(manager->isTakeoverMuted());
		QCOMPARE(manager->getTakeoverScalar(), 0.5);
	}
	void queuedKeyCannotCrossTakeoverGeneration()
	{
		hook(VK_VOLUME_UP, 6000);
		++manager->mediaKeyGeneration;
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.5);
	}
	void staleHotkeyFromPriorActivationIsDiscarded()
	{
		manager->mediaKeyEnableTime = 8000;
		hotkey(0, 7999);
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.5);
		hotkey(0, 8001);
		drain();
		QCOMPARE(manager->getTakeoverScalar(), 0.52);
	}
	void sliderIsAbsoluteRegardlessOfKeyStep()
	{
		manager->setVolumeStepPercent(5);
		manager->setVolumeScalar(0.371);
		QCOMPARE(manager->getTakeoverScalar(), 0.37);
		manager->stepVolume(true);
		QCOMPARE(manager->getTakeoverScalar(), 0.42);
	}
	void nextPhysicalDropIsCountedAfterOwnReset()
	{
		testEndpoint.scalar = 0.98;
		manager->checkVolumeChange();
		QCOMPARE(manager->getTakeoverScalar(), 0.48);
		// Another distinct drop arrives before the poll sees the reset to 100%.
		testEndpoint.scalar = 0.98;
		manager->checkVolumeChange();
		QCOMPARE(manager->getTakeoverScalar(), 0.46);
	}
	void failedResetDoesNotRepeatPhysicalDrop()
	{
		testEndpointWriteFails = true;
		testEndpoint.scalar = 0.98;
		manager->checkVolumeChange();
		manager->checkVolumeChange();
		manager->checkVolumeChange();
		QCOMPARE(manager->getTakeoverScalar(), 0.48);
	}
};
QTEST_MAIN(VolumeTakeoverTests)
#include "volume-takeover.moc"
