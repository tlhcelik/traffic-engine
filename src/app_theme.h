#ifndef APP_THEME_H
#define APP_THEME_H

#include <QColor>

class QApplication;

// app_theme
// --------
// The colors of the program. All colors are pastel and they match: the user
// interface (Fusion style + palette + style sheet), the tool icons and the
// objects in the OpenGL world use the same set.

namespace app_theme
{
	// Sets the Fusion style, the palette and the style sheet.
	void apply(QApplication &app);

	// The pastel set (index wraps around): mint, sky, peach, lavender, lemon,
	// coral, sage, rose, aqua, sand.
	QColor pastel(int index);
	int pastel_count();

	// Fixed colors
	QColor text_dark();      // dark slate, for glyphs and text
	QColor panel_bg();       // background of the panels in the OpenGL world
	QColor accent();         // selection and focus (pastel blue)
	QColor warning();        // pastel orange
	QColor danger();         // pastel red
	QColor ok();             // pastel green
	// Level of service A (green) to F (red): congestion analysis
	QColor los_color(char los);
}

#endif // APP_THEME_H
