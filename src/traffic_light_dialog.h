#ifndef TRAFFIC_LIGHT_DIALOG_H
#define TRAFFIC_LIGHT_DIALOG_H

#include "world_model.h"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QTableWidget;
class workspace;
class light_timeline;

// traffic_light_dialog
// --------------------
// The program of one traffic light: the algorithm (fixed time, actuated,
// external), the start offset, the red + yellow time and the phases. The
// user puts the phases in sequence (up, down), sets the green, yellow and
// all-red times and the roads with green for each phase. A timeline shows
// one full cycle. Apply writes the program to the model; the simulation
// uses it immediately.

class traffic_light_dialog : public QDialog
{
	Q_OBJECT

public:
	traffic_light_dialog(workspace *ws, int light_id, QWidget *parent = nullptr);

private:
	void fill_table();
	void fill_roads();
	void update_summary();
	bool apply();
	int current_phase() const;

	workspace *m_ws;
	int m_light_id;
	light_data m_edit;
	QComboBox *m_algorithm;
	QDoubleSpinBox *m_offset;
	QDoubleSpinBox *m_red_yellow;
	QTableWidget *m_table;
	QListWidget *m_roads;
	QLabel *m_cycle;
	light_timeline *m_timeline;
	bool m_filling;
};

#endif // TRAFFIC_LIGHT_DIALOG_H
