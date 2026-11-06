#ifndef CONGESTION_DOCK_H
#define CONGESTION_DOCK_H

#include "simulation.h"
#include "world_model.h"

#include <QColor>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class workspace;

// congestion_dock
// ---------------
// Congestion analysis for the analyst: the junctions of the current workspace
// with their level of service (LOS A to F), delay per vehicle, queue, flow
// and the main cause of the waits (red light, priority, full exit, ...), the
// worst first. The approaches (incoming roads) are the children of each
// junction. A click on a row shows the junction or the road (go_to). The
// map view "Congestion analysis" shows the same data on the roads.
// Time window: 1, 5 or 15 minutes. Export: CSV.

class congestion_dock : public QWidget
{
	Q_OBJECT

public:
	explicit congestion_dock(QWidget *parent = nullptr);

	void set_workspace(workspace *ws);
	void refresh();
	// The texts from the code again (language change)
	void retranslate();

	static QColor los_color(char los);
	// The main cause as a short text (language of the user interface)
	static QString cause_text(wait_cause cause);
	// "Node 12 - Ataturk Avenue / Station Road"
	static QString junction_name(const world_model &m, int node_id);

signals:
	void go_to(const element_ref &ref);

private:
	void export_csv();

	workspace *m_ws;
	QLabel *m_summary;
	QComboBox *m_window;
	QCheckBox *m_problems;
	QTreeWidget *m_tree;
};

#endif // CONGESTION_DOCK_H
