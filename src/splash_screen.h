#ifndef SPLASH_SCREEN_H
#define SPLASH_SCREEN_H

#include <QPixmap>
#include <QWidget>

class QPropertyAnimation;
class QTimer;

// splash_screen
// -------------
// The start screen (the same flow as in Asan Map). A frameless window in the
// middle of the screen:
//   1. fade in (window opacity 0 -> 1, fade_ms)
//   2. ready_to_init(): main() makes the main window now (the slow part). A
//      timer for the minimum visible time (hold_ms) starts at the same time:
//      no flash on a fast computer.
//   3. main() calls finish() at the end of the start. When the minimum time
//      is also over: fade out (1 -> 0).
//   4. finished(): the splash closes; main() shows the main window.
// The card is drawn (no image file): icon, name, a small road with moving
// vehicles and a traffic light, status text, version and a progress bar.

class splash_screen : public QWidget
{
	Q_OBJECT

public:
	splash_screen(int fade_ms, int hold_ms, QWidget *parent = nullptr);

	void start();
	// One line of status above the progress bar ("Open the workspace...").
	// The event loop does not run during the start: the splash paints now.
	void set_status(const QString &text);
	// The start of the main window is complete.
	void finish();

signals:
	void ready_to_init();
	void finished();

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	void maybe_fade_out();
	void fade(double from, double to);

	int m_fade_ms;
	int m_hold_ms;
	QPropertyAnimation *m_anim;
	QTimer *m_tick;
	QPixmap m_icon;
	double m_phase;        // 0..1, the animation
	QString m_status;
	bool m_hold_done;
	bool m_init_done;
	bool m_fading_out;
};

#endif // SPLASH_SCREEN_H
