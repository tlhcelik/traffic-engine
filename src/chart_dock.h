#ifndef CHART_DOCK_H
#define CHART_DOCK_H

#include <QWidget>

#include <vector>

class QComboBox;
class QListWidget;
class QVBoxLayout;
class chart_widget;
class workspace;

// chart_dock
// ----------
// The charts of the simulation. The user selects the metrics (each metric
// makes one chart), the time window and optionally a traffic light (waiting
// vehicles at that light). The charts update every second. Export: PNG and
// SVG (all charts in one image) and CSV (all data).

class chart_dock : public QWidget
{
	Q_OBJECT

public:
	explicit chart_dock(QWidget *parent = nullptr);

	void set_workspace(workspace *ws);
	void refresh();
	// The texts from the code again (language change): metrics, lights, charts.
	void retranslate();

private:
	void rebuild_charts();
	void fill_lights();
	void export_image(bool svg);
	void export_csv();

	workspace *m_ws;
	QListWidget *m_metrics;
	QComboBox *m_window;
	QComboBox *m_light;
	QWidget *m_host;
	QVBoxLayout *m_charts_layout;
	std::vector<chart_widget *> m_charts;
	std::vector<int> m_chart_metric;   // metric index, or -1 for the light chart
	quint64 m_lights_revision;
};

#endif // CHART_DOCK_H
