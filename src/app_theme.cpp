#include "app_theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace
{
	const QColor g_pastels[] =
	{
		QColor(0xA8, 0xE6, 0xCF), // mint
		QColor(0xA7, 0xC7, 0xE7), // sky
		QColor(0xFF, 0xD3, 0xB6), // peach
		QColor(0xD5, 0xC6, 0xF2), // lavender
		QColor(0xFD, 0xF1, 0xA6), // lemon
		QColor(0xFF, 0xAA, 0xA5), // coral
		QColor(0xC9, 0xE4, 0xB4), // sage
		QColor(0xF7, 0xC6, 0xD9), // rose
		QColor(0xA6, 0xE3, 0xE9), // aqua
		QColor(0xE8, 0xD8, 0xB8), // sand
	};

	const char *g_style_sheet = R"(
QMainWindow, QDialog { background: #F4F6FA; }
QToolBar { background: #FFFFFF; border: none; spacing: 4px; padding: 4px; }
QToolBar#tool_palette { border-right: 1px solid #E1E6EE; }
QToolBar#main_toolbar { border-bottom: 1px solid #E1E6EE; }
QToolButton { border: 1px solid transparent; border-radius: 9px; padding: 3px; }
QToolButton:hover { background: #EAF2FB; border-color: #C9DCF0; }
QToolButton:checked { background: #DCEAF8; border-color: #A7C7E7; }
QToolButton:pressed { background: #CFE2F5; }
QTabWidget::pane { border: none; }
QTabBar::tab { background: #E9EDF3; color: #3D4A5C; border-top-left-radius: 9px; border-top-right-radius: 9px;
	padding: 6px 14px; margin-right: 2px; }
QTabBar::tab:selected { background: #FFFFFF; font-weight: bold; }
QTabBar::tab:hover:!selected { background: #DCEAF8; }
QDockWidget { titlebar-close-icon: none; }
QDockWidget::title { background: #E6EEF7; padding: 6px; border-top-left-radius: 6px; border-top-right-radius: 6px; }
QGroupBox { border: 1px solid #DDE3EC; border-radius: 9px; margin-top: 12px; background: #FFFFFF; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #3D4A5C; }
QPushButton { background: #EAF2FB; border: 1px solid #C9DCF0; border-radius: 8px; padding: 5px 14px; }
QPushButton:hover { background: #DCEAF8; }
QPushButton:pressed { background: #CFE2F5; }
QPushButton:default { border-color: #8DB4DE; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTimeEdit { border: 1px solid #D3DAE4; border-radius: 6px; padding: 2px 6px; background: #FFFFFF; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: #8DB4DE; }
QTreeWidget, QListWidget, QTableWidget { border: 1px solid #DDE3EC; border-radius: 8px; background: #FFFFFF; alternate-background-color: #F6F8FB; }
QHeaderView::section { background: #EEF2F7; border: none; padding: 4px; color: #3D4A5C; }
QStatusBar { background: #FFFFFF; border-top: 1px solid #E1E6EE; color: #3D4A5C; }
QSlider::groove:horizontal { height: 6px; background: #E1E6EE; border-radius: 3px; }
QSlider::handle:horizontal { background: #A7C7E7; border: 1px solid #8DB4DE; width: 14px; margin: -5px 0; border-radius: 7px; }
QSlider::sub-page:horizontal { background: #C9DCF0; border-radius: 3px; }
QMenu { background: #FFFFFF; border: 1px solid #DDE3EC; border-radius: 8px; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 24px; border-radius: 6px; }
QMenu::item:selected { background: #DCEAF8; color: #1F2A37; }
QToolTip { background: #FFFDE7; color: #2B3445; border: 1px solid #E8DFA8; border-radius: 6px; padding: 4px; }
)";
}

namespace app_theme
{
	void apply(QApplication &app)
	{
		app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
		QPalette p;
		p.setColor(QPalette::Window, QColor(0xF4, 0xF6, 0xFA));
		p.setColor(QPalette::WindowText, QColor(0x2B, 0x34, 0x45));
		p.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
		p.setColor(QPalette::AlternateBase, QColor(0xF6, 0xF8, 0xFB));
		p.setColor(QPalette::Text, QColor(0x2B, 0x34, 0x45));
		p.setColor(QPalette::Button, QColor(0xEE, 0xF1, 0xF6));
		p.setColor(QPalette::ButtonText, QColor(0x2B, 0x34, 0x45));
		p.setColor(QPalette::Highlight, QColor(0xA7, 0xC7, 0xE7));
		p.setColor(QPalette::HighlightedText, QColor(0x1F, 0x2A, 0x37));
		p.setColor(QPalette::ToolTipBase, QColor(0xFF, 0xFD, 0xE7));
		p.setColor(QPalette::ToolTipText, QColor(0x2B, 0x34, 0x45));
		p.setColor(QPalette::Link, QColor(0x4A, 0x7F, 0xB8));
		p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9A, 0xA3, 0xB0));
		p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x9A, 0xA3, 0xB0));
		app.setPalette(p);
		app.setStyleSheet(QString::fromLatin1(g_style_sheet));
	}

	QColor pastel(int index)
	{
		const int n = pastel_count();
		return g_pastels[((index % n) + n) % n];
	}

	int pastel_count()
	{
		return static_cast<int>(sizeof(g_pastels) / sizeof(g_pastels[0]));
	}

	QColor text_dark() { return QColor(0x3D, 0x4A, 0x5C); }
	QColor panel_bg() { return QColor(0xFF, 0xFF, 0xFF, 235); }
	QColor accent() { return QColor(0xA7, 0xC7, 0xE7); }
	QColor warning() { return QColor(0xFF, 0xC8, 0x8A); }
	QColor danger() { return QColor(0xFF, 0x8A, 0x80); }
	QColor ok() { return QColor(0x8F, 0xD1, 0x9E); }

	QColor los_color(char los)
	{
		switch (los)
		{
			case 'A': return QColor(102, 187, 106);
			case 'B': return QColor(156, 204, 101);
			case 'C': return QColor(253, 216, 53);
			case 'D': return QColor(255, 167, 38);
			case 'E': return QColor(244, 81, 30);
			default: return QColor(198, 40, 40);
		}
	}
}
