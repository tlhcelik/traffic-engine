#ifndef TRAFFIC_DENSITY_WIDGET_H
#define TRAFFIC_DENSITY_WIDGET_H

#include <QWidget>

class QSlider;
class QSpinBox;

// traffic_density_widget
// ----------------------
// The global traffic density (toolbar button "Traffic", menu Simulation >
// Traffic density). Two values in percent of the rates of the traffic
// sources (the node properties "Vehicles" and "Pedestrians"):
//   simulation/traffic_density     vehicles
//   simulation/pedestrian_density  pedestrians
// 100 % = the rates of the nodes, 0 % = no new traffic, 300 % = three times
// the rates. The values are in config.xml. They apply immediately to all open
// workspaces: each workspace reads them when config_common sends changed().

class traffic_density_widget : public QWidget
{
	Q_OBJECT

public:
	explicit traffic_density_widget(QWidget *parent = nullptr);

	// Text and tool tip of the toolbar button, for example "Traffic 100%".
	static QString button_text();
	static QString button_tip();
	// Reads the values from config.xml again (for example after the settings dialog).
	void reload();

private:
	struct row
	{
		QString key;
		QSlider *slider = nullptr;
		QSpinBox *spin = nullptr;
	};

	void set_percent(int index, int percent);

	row m_rows[2];
};

#endif // TRAFFIC_DENSITY_WIDGET_H
