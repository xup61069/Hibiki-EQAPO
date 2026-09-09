// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QApplication;
class QWidget;

// Presentation only. Never delays a signal or changes a control's model value.
namespace StudioMotion
{
	void install(QApplication& application);
	bool allowed();
	void reveal(QWidget* widget);
	void feedback(QWidget* widget);
}
