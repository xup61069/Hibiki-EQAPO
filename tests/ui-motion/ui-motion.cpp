// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QDial>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QVariantAnimation>
#include "Editor/StudioMotion.h"
#include "Editor/widgets/StudioSignature.h"

class MotionTests final : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		QApplication::setStyle(QStringLiteral("Fusion"));
		StudioMotion::install(*qApp);
		StudioMotion::install(*qApp);
	}

	void cleanup()
	{
		qApp->setProperty("eqapoDisableAnimations", false);
		qApp->setProperty("eqapoModernThemeHighContrast", false);
		qunsetenv("EQAPO_UI_SNAPSHOT");
		qunsetenv("EQAPO_DISABLE_ANIMATIONS");
	}

	void accessibilityAndSnapshotGates()
	{
		qApp->setProperty("eqapoDisableAnimations", true);
		QVERIFY(!StudioMotion::allowed());
		qApp->setProperty("eqapoDisableAnimations", false);
		qApp->setProperty("eqapoModernThemeHighContrast", true);
		QVERIFY(!StudioMotion::allowed());
		qApp->setProperty("eqapoModernThemeHighContrast", false);
		qputenv("EQAPO_UI_SNAPSHOT", "capture.png");
		QVERIFY(!StudioMotion::allowed());
		qunsetenv("EQAPO_UI_SNAPSHOT");
		qputenv("EQAPO_DISABLE_ANIMATIONS", "1");
		QVERIFY(!StudioMotion::allowed());
	}

	void inputRemainsImmediate()
	{
		QPushButton button(QStringLiteral("Compare"));
		button.setCheckable(true);
		button.resize(140, 40);
		button.show();
		QTest::qWait(20);
		QSignalSpy clicks(&button, &QPushButton::clicked);
		StudioMotion::feedback(&button);
		QTest::mouseClick(&button, Qt::LeftButton);
		QCOMPARE(clicks.count(), 1);
		QVERIFY(button.isChecked());
		QTest::keyClick(&button, Qt::Key_Space);
		QCOMPARE(clicks.count(), 2);
		QVERIFY(!button.isChecked());
	}

	void burstsHaveBoundedLifetime()
	{
		if (!StudioMotion::allowed()) QSKIP("Windows client-area animations are disabled");
		QPushButton button(QStringLiteral("Save"));
		button.show();
		for (int i = 0; i < 100; ++i) StudioMotion::feedback(&button);
		const auto overlays = button.findChildren<QWidget*>(QStringLiteral("studioMotionOverlay"));
		QCOMPARE(overlays.size(), 1);
		QVERIFY(overlays.front()->testAttribute(Qt::WA_TransparentForMouseEvents));
		QVERIFY(overlays.front()->isVisible());
		QTest::qWait(600);
		QVERIFY(!overlays.front()->isVisible());
		for (auto* tween : button.findChildren<QVariantAnimation*>())
			QCOMPARE(tween->state(), QAbstractAnimation::Stopped);
	}

	void deletionCancelsOwnedAnimations()
	{
		if (!StudioMotion::allowed()) QSKIP("Windows client-area animations are disabled");
		auto* button = new QPushButton(QStringLiteral("Temporary"));
		button->show();
		StudioMotion::reveal(button);
		QPointer<QWidget> overlay = button->findChild<QWidget*>(QStringLiteral("studioMotionOverlay"));
		QVERIFY(overlay);
		delete button;
		QVERIFY(overlay.isNull());
		QTest::qWait(50);
	}

	void hiddenControlsStopImmediately()
	{
		QPushButton button(QStringLiteral("Hide"));
		button.show();
		StudioMotion::feedback(&button);
		button.hide();
		for (auto* tween : button.findChildren<QVariantAnimation*>())
			QCOMPARE(tween->state(), QAbstractAnimation::Stopped);
	}

	void reduceMotionDuringTransition()
	{
		if (!StudioMotion::allowed()) QSKIP("Windows client-area animations are disabled");
		QPushButton button(QStringLiteral("Save"));
		button.show();
		StudioMotion::feedback(&button);
		qApp->setProperty("eqapoDisableAnimations", true);
		QTest::qWait(60);
		auto* overlay = button.findChild<QWidget*>(QStringLiteral("studioMotionOverlay"));
		QVERIFY(overlay && !overlay->isVisible());
	}

	void dialModelDoesNotWaitForPresentation()
	{
		QDial dial;
		dial.setRange(0, 1000);
		dial.show();
		QTest::qWait(20);
		QSignalSpy changes(&dial, &QDial::valueChanged);
		dial.setValue(800);
		QCOMPARE(dial.value(), 800);
		QCOMPARE(changes.count(), 1);
		dial.setValue(200);
		QCOMPARE(dial.value(), 200);
		QTest::qWait(220);
		QCOMPARE(dial.property("studioDialValue").toReal(), 200.0);
		QCOMPARE(changes.count(), 2);
	}

	void signatureStopsWhenHidden()
	{
		StudioSignature signature;
		signature.show();
		QTest::qWait(20);
		signature.hide();
		for (auto* tween : signature.findChildren<QVariantAnimation*>())
			QCOMPARE(tween->state(), QAbstractAnimation::Stopped);
	}
};

QTEST_MAIN(MotionTests)
#include "ui-motion.moc"
