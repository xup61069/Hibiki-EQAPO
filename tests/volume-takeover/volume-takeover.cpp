#include <QTest>
#include <QSignalSpy>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "Editor/helpers/VolumeTakeoverManager.h"
#include "filters/loudnessCorrection/HibikiVolumeTakeoverShared.h"

extern EndpointVolumeState testEndpoint;
extern bool testEndpointWriteFails;
extern HibikiVolumeTakeoverSharedData* testSharedState;
extern bool testRaisedEndpointBeforePublish;
extern bool testReleasedBeforeEndpointRestore;
extern bool testMuteWriteFails;

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
	void restartTakeover()
	{
		manager->takeoverEnabled = false;
		// Suppress live mapping creation; all IPC remains inside this fixture.
		manager->takeoverMapping = reinterpret_cast<HANDLE>(1);
		manager->setTakeoverEnabled(true);
	}
private slots:
	void init()
	{
		testEndpoint = {};
		testEndpointWriteFails = false;
		testRaisedEndpointBeforePublish = false;
		testReleasedBeforeEndpointRestore = false;
		testMuteWriteFails = false;
		shared = {};
		testSharedState = &shared;
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
		manager->takeoverMapping = nullptr;
		testSharedState = nullptr;
		manager->s_keyboardHook = nullptr;
		for (bool& value : manager->mediaHotkeyRegistered) value = false;
		delete manager;
		manager = nullptr;
	}
	void rebootRestoresLastSharedVolumeInsteadOfPhysical100()
	{
		shared.version = 1;
		shared.active = 1;
		shared.scalar = 0.37;
		shared.levelDb = -63.0;
		// Uptime from the previous boot must not invalidate the remembered volume.
		shared.lastHeartbeatTick = GetTickCount64() + 100000;
		testEndpoint.scalar = 1.0;
		restartTakeover();
		QCOMPARE(manager->getTakeoverScalar(), 0.37);
		QCOMPARE(shared.scalar, 0.37);
	}
	void startupPublishesAttenuationBeforeRaisingPhysicalVolume()
	{
		testEndpoint.scalar = 0.23;
		testEndpoint.levelDb = -77.0;
		restartTakeover();
		QCOMPARE(shared.scalar, 0.23);
		QVERIFY(!testRaisedEndpointBeforePublish);
	}
	void everyAdjustmentIsSavedWithoutWaitingForExit()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const QString path = directory.filePath("volume.ini");
		connect(manager, &VolumeTakeoverManager::takeoverVolumeRemembered, this,
			[path](const QByteArray& value) {
				QSettings settings(path, QSettings::IniFormat);
				settings.setValue("takeoverVolumeState", QString::fromUtf8(value));
				settings.sync();
			});
		manager->setVolumeScalar(0.31);
		manager->toggleMute();
		QSettings reader(path, QSettings::IniFormat);
		const QByteArray saved = reader.value("takeoverVolumeState").toByteArray();
		const QJsonObject record = QJsonDocument::fromJson(saved).object();
		QCOMPARE(record.value("scalar").toDouble(), 0.31);
		QVERIFY(record.value("muted").toBool());
		// Model a reboot with the physical endpoint left at full volume.
		testEndpoint.scalar = 1.0;
		manager->takeoverEnabled = false;
		QVERIFY(manager->restoreTakeoverVolume(saved));
		restartTakeover();
		QCOMPARE(manager->getTakeoverScalar(), 0.31);
		QVERIFY(manager->isTakeoverMuted());
	}
	void savedVolumeOverridesAnOlderSharedSnapshot()
	{
		shared.version = 1;
		shared.active = 1;
		shared.scalar = 1.0;
		shared.levelDb = 0.0;
		QVERIFY(manager->restoreTakeoverVolume(R"({"version":1,"scalar":0.42,"levelDb":-58,"muted":false})"));
		restartTakeover();
		QCOMPARE(manager->getTakeoverScalar(), 0.42);
	}
	void manualStartupRestoresVolumeAndMuteAfterProfileLoad()
	{
		manager->setManualMode(true, 0.0);
		QVERIFY(manager->restoreTakeoverVolume(R"({"version":1,"scalar":0.24,"levelDb":-76,"muted":true})"));
		restartTakeover();
		QCOMPARE(manager->getManualVolumeDb(), -76.0);
		QCOMPARE(manager->getTakeoverScalar(), 0.24);
		QVERIFY(manager->isTakeoverMuted());
		manager->stepVolume(true);
		QCOMPARE(manager->getTakeoverScalar(), 0.26);
		QVERIFY(!manager->isTakeoverMuted());
	}
	void invalidSavedVolumeStartsMuted_data()
	{
		QTest::addColumn<QByteArray>("value");
		for (const char* text : {"not JSON", "{}", R"({"version":2,"scalar":0.3,"levelDb":-70,"muted":false})",
			R"({"version":1,"scalar":1.1,"levelDb":-70,"muted":false})",
			R"({"version":1,"scalar":0.3,"levelDb":5,"muted":false})",
			R"({"version":1,"scalar":0.3,"levelDb":-70,"muted":"false"})"})
			QTest::newRow(text) << QByteArray(text);
	}
	void manualStartupConvertsAutomaticEndpointDbWithoutJumping()
	{
		manager->setManualMode(true, 0.0);
		QVERIFY(manager->restoreTakeoverVolume(R"({"version":1,"scalar":0.37,"levelDb":-20,"muted":false})"));
		restartTakeover();
		QCOMPARE(manager->getManualVolumeDb(), -63.0);
		manager->stepVolume(true);
		QCOMPARE(manager->getTakeoverScalar(), 0.39);
	}
	void invalidSavedVolumeStartsMuted()
	{
		QFETCH(QByteArray, value);
		QVERIFY(!manager->restoreTakeoverVolume(value));
		restartTakeover();
		QCOMPARE(manager->getTakeoverScalar(), 0.0);
		QVERIFY(manager->isTakeoverMuted());
		manager->stepVolume(true);
		QCOMPARE(manager->getTakeoverScalar(), 0.02);
		QVERIFY(!manager->isTakeoverMuted());
	}
	void unchangedPollingDoesNotRewriteSavedVolume()
	{
		QSignalSpy saved(manager, &VolumeTakeoverManager::takeoverVolumeRemembered);
		manager->setVolumeScalar(0.4);
		manager->publishTakeoverSharedData();
		manager->checkVolumeChange();
		QCOMPARE(saved.size(), 1);
	}
	void releaseKeepsAttenuationUntilEndpointRestored_data()
	{
		QTest::addColumn<int>("failure");
		QTest::newRow("success") << 0;
		QTest::newRow("scalar-failure") << 1;
		QTest::newRow("mute-failure") << 2;
	}
	void releaseKeepsAttenuationUntilEndpointRestored()
	{
		QFETCH(int, failure);
		// Anonymous mappings exercise the actual release path without live IPC.
		HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			0, sizeof(shared), nullptr);
		QVERIFY(mapping != nullptr);
		auto* writer = static_cast<HibikiVolumeTakeoverSharedData*>(
			MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shared)));
		auto* reader = static_cast<const HibikiVolumeTakeoverSharedData*>(
			MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(shared)));
		QVERIFY(writer && reader);
		auto readerGuard = qScopeGuard([reader]() { UnmapViewOfFile(reader); });
		*writer = shared;
		writer->active = 1;
		writer->scalar = 0.5;
		writer->muted = 1;
		manager->takeoverMapping = mapping;
		manager->takeoverShared = writer;
		manager->takeoverMuted = true;
		testSharedState = writer;
		testEndpoint.scalar = 1.0;
		testEndpointWriteFails = failure == 1;
		testMuteWriteFails = failure == 2;
		manager->releaseTakeoverVolume();
		testSharedState = nullptr;
		QCOMPARE(reader->active, failure == 0 ? 0u : 1u);
		QCOMPARE(testEndpoint.scalar, failure == 1 ? 1.0 : 0.5);
		QVERIFY(!testReleasedBeforeEndpointRestore);
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
