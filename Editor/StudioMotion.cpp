// SPDX-License-Identifier: GPL-2.0-or-later
#include "StudioMotion.h"

#include <QAbstractButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QWheelEvent>
#include <QComboBox>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDial>
#include <QEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QVariantAnimation>
#include <cmath>
#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

namespace
{
	// Only ordinary scroll areas participate. Plot wheel gestures retain zoom,
	// and precision trackpads retain their own native momentum.
	class SmoothScroll final : public QObject
	{
	public:
		explicit SmoothScroll(QScrollArea* owner) : QObject(owner), area(owner), tween(this)
		{
			setObjectName(QStringLiteral("studioSmoothScroll"));
			owner->viewport()->installEventFilter(this);
			owner->installEventFilter(this);
			bar = owner->verticalScrollBar();
			bar->installEventFilter(this);
			tween.setDuration(210);
			tween.setEasingCurve(QEasingCurve::OutCubic);
			connect(&tween, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
				if (!StudioMotion::allowed() || !area->isVisible()) { tween.stop(); return; }
				writing = true;
				bar->setValue(value.toInt());
				writing = false;
			});
			connect(bar, &QScrollBar::valueChanged, this, [this](int) {
				if (!writing) tween.stop(); // Keyboard, dragging and programmatic navigation win.
			});
			connect(bar, &QScrollBar::rangeChanged, this, [this](int, int) { tween.stop(); });
		}
	protected:
		bool eventFilter(QObject* object, QEvent* event) override
		{
			if (event->type() == QEvent::Hide || event->type() == QEvent::MouseButtonPress
				|| event->type() == QEvent::KeyPress)
				tween.stop();
			if (event->type() != QEvent::Wheel || object != area->viewport()) return false;
			auto* wheel = static_cast<QWheelEvent*>(event);
			if (!StudioMotion::allowed() || !wheel->pixelDelta().isNull()
				|| wheel->modifiers() != Qt::NoModifier || wheel->angleDelta().y() == 0)
			{ tween.stop(); return false; }
			const int start = bar->value();
			const int previous = tween.state() == QAbstractAnimation::Running ? destination : start;
			const int distance = qRound(wheel->angleDelta().y() / 120.0
				* QApplication::wheelScrollLines() * qMax(bar->singleStep(), area->fontMetrics().height()));
			destination = qBound(bar->minimum(), previous - distance, bar->maximum());
			if (destination == start && tween.state() != QAbstractAnimation::Running) return false;
			tween.stop();
			tween.setStartValue(start);
			tween.setEndValue(destination);
			tween.start();
			wheel->accept();
			return true;
		}
	private:
		QScrollArea* area;
		QScrollBar* bar;
		QVariantAnimation tween;
		int destination = 0;
		bool writing = false;
	};

	// One bounded animation per visible control; no permanent frame timer.
	// A child overlay keeps native input, focus, layout and accessibility intact.
	class MotionOverlay final : public QWidget
	{
	public:
		explicit MotionOverlay(QWidget* target) : QWidget(target), animation(this)
		{
			setObjectName(QStringLiteral("studioMotionOverlay"));
			setAttribute(Qt::WA_TransparentForMouseEvents);
			setAttribute(Qt::WA_NoSystemBackground);
			setFocusPolicy(Qt::NoFocus);
			animation.setStartValue(0.0);
			animation.setEndValue(1.0);
			animation.setEasingCurve(QEasingCurve::OutCubic);
			connect(&animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
				if (!StudioMotion::allowed() || !parentWidget()->isVisible())
				{
					animation.stop();
					hide();
					return;
				}
				progress = value.toReal();
				update();
			});
			connect(&animation, &QVariantAnimation::finished, this, [this]() { hide(); });
			target->installEventFilter(this);
			hide();
		}

		void play(bool entrance, QPointF position = QPointF())
		{
			animation.stop();
			if (!StudioMotion::allowed() || !parentWidget()->isVisible())
			{
				hide();
				return;
			}
			curtain = entrance;
			origin = position.isNull() ? QRectF(parentWidget()->rect()).center() : position;
			progress = 0;
			setGeometry(parentWidget()->rect());
			animation.setDuration(entrance ? 360 : 460);
			show();
			raise();
			animation.start();
		}

	protected:
		bool eventFilter(QObject*, QEvent* event) override
		{
			if (event->type() == QEvent::Resize)
				setGeometry(parentWidget()->rect());
			if (event->type() == QEvent::Hide || event->type() == QEvent::EnabledChange)
			{
				animation.stop();
				hide();
			}
			return false;
		}

		void paintEvent(QPaintEvent*) override
		{
			QPainter painter(this);
			painter.setRenderHint(QPainter::Antialiasing);
			QPainterPath clip;
			clip.addRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 7, 7);
			painter.setClipPath(clip);
			QColor accent = palette().color(QPalette::Highlight);
			if (curtain)
			{
				QColor surface = palette().color(QPalette::Window);
				surface.setAlphaF(0.86 * (1.0 - progress));
				painter.fillRect(rect(), surface);
				accent.setAlphaF(0.2 * (1.0 - progress));
				painter.fillRect(QRectF(0, 0, width() * progress, 2), accent);
			}
			else
			{
				accent.setAlphaF(0.16 * (1.0 - progress));
				painter.setBrush(accent);
				painter.setPen(Qt::NoPen);
				const qreal radius = std::hypot(width(), height()) * progress;
				painter.drawEllipse(origin, radius, radius);
				accent.setAlphaF(0.55 * (1.0 - progress));
				painter.setBrush(Qt::NoBrush);
				painter.setPen(QPen(accent, 1.2));
				painter.drawPath(clip);
			}
		}

	private:
		QVariantAnimation animation;
		QPointF origin;
		qreal progress = 0;
		bool curtain = false;
	};

	MotionOverlay* overlayFor(QWidget* target)
	{
		// No raw-pointer cache: the target owns its overlay and all connections.
		for (QObject* child : target->children())
			if (child->objectName() == QStringLiteral("studioMotionOverlay"))
				return static_cast<MotionOverlay*>(child);
		return new MotionOverlay(target);
	}

	class MotionMonitor final : public QObject
	{
	public:
		explicit MotionMonitor(QApplication& application) : QObject(&application)
		{
			application.installEventFilter(this);
		}

	protected:
		bool eventFilter(QObject* object, QEvent* event) override
		{
			const auto type = event->type();
			if (type != QEvent::Show && type != QEvent::Enter && type != QEvent::FocusIn
				&& type != QEvent::MouseButtonPress && type != QEvent::Polish)
				return false;
			QWidget* widget = qobject_cast<QWidget*>(object);
			if (!widget || widget->objectName() == QStringLiteral("studioMotionOverlay"))
				return false;

			if (type == QEvent::Polish)
			{
				if (widget->property("studioMotionConnected").toBool())
					return false;
				widget->setProperty("studioMotionConnected", true);
				if (auto* area = qobject_cast<QScrollArea*>(widget))
					new SmoothScroll(area);
				if (auto* button = qobject_cast<QAbstractButton*>(widget))
					connect(button, &QAbstractButton::toggled, button, [button](bool) {
						StudioMotion::feedback(button);
					});
				if (auto* combo = qobject_cast<QComboBox*>(widget))
					connect(combo, &QComboBox::activated, combo, [combo](int) {
						StudioMotion::feedback(combo);
					});
				if (auto* tabs = qobject_cast<QTabBar*>(widget))
					connect(tabs, &QTabBar::currentChanged, tabs, [tabs](int) {
						StudioMotion::feedback(tabs);
					});
				if (auto* dial = qobject_cast<QDial*>(widget))
				{
					auto* tween = new QVariantAnimation(dial);
					tween->setDuration(140);
					tween->setEasingCurve(QEasingCurve::OutCubic);
					connect(tween, &QVariantAnimation::valueChanged, dial, [dial, tween](const QVariant& value) {
						if (!StudioMotion::allowed() || !dial->isVisible())
						{
							tween->stop();
							dial->setProperty("studioDialValue", dial->value());
						}
						else
							dial->setProperty("studioDialValue", value);
						dial->update();
					});
					connect(dial, &QDial::valueChanged, dial, [dial, tween](int value) {
						const QVariant previous = dial->property("studioDialValue");
						tween->stop();
						if (!previous.isValid() || !StudioMotion::allowed() || !dial->isVisible() || dial->isSliderDown())
							dial->setProperty("studioDialValue", value);
						else
						{
							tween->setStartValue(previous.toReal());
							tween->setEndValue(qreal(value));
							tween->start();
						}
					});
					dial->setProperty("studioDialValue", dial->value());
				}
				return false;
			}

			if (!widget->isEnabled() || !StudioMotion::allowed())
				return false;
			if (type == QEvent::Show)
			{
				if (qobject_cast<QMenu*>(widget) || widget->inherits("FilterTableRow")
					|| widget->objectName() == QStringLiteral("studioHeader"))
					QTimer::singleShot(0, widget, [widget]() { StudioMotion::reveal(widget); });
			}
			else if (qobject_cast<QAbstractButton*>(widget) || qobject_cast<QDial*>(widget)
				|| qobject_cast<QComboBox*>(widget) || qobject_cast<QAbstractSpinBox*>(widget)
				|| (type == QEvent::FocusIn && qobject_cast<QLineEdit*>(widget)))
			{
				const QPointF point = type == QEvent::MouseButtonPress
					? static_cast<QMouseEvent*>(event)->position() : QPointF();
				overlayFor(widget)->play(false, point);
			}
			return false;
		}
	};
}

bool StudioMotion::allowed()
{
	if (!qApp || qApp->property("eqapoDisableAnimations").toBool()
		|| qApp->property("eqapoModernThemeHighContrast").toBool()
		|| qEnvironmentVariableIsSet("EQAPO_UI_SNAPSHOT")
		|| qEnvironmentVariableIsSet("EQAPO_DISABLE_ANIMATIONS"))
		return false;
#ifdef Q_OS_WIN
	BOOL enabled = TRUE;
	if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && !enabled)
		return false;
#endif
	return qApp->style() && qApp->style()->styleHint(QStyle::SH_Widget_Animate);
}

void StudioMotion::install(QApplication& application)
{
	if (application.property("studioMotionInstalled").toBool())
		return;
	application.setProperty("studioMotionInstalled", true);
	new MotionMonitor(application);
}

void StudioMotion::reveal(QWidget* widget)
{
	if (widget && widget->isVisible() && allowed())
		overlayFor(widget)->play(true);
}

void StudioMotion::feedback(QWidget* widget)
{
	if (widget && widget->isVisible() && allowed())
		overlayFor(widget)->play(false);
}
