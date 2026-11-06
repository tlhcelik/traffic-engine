#ifndef PROPERTY_PANEL_H
#define PROPERTY_PANEL_H

#include "world_model.h"

#include <QWidget>

#include <functional>
#include <vector>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class workspace;

// property_panel
// --------------
// Shows the properties of the selected element (or of the workspace if
// nothing is selected). The editors come from world_properties. A change
// goes to the model immediately; the simulation uses it at runtime.
//
// For vehicles and pedestrians, the panel shows live data (read only).

class property_panel : public QWidget
{
	Q_OBJECT

public:
	explicit property_panel(QWidget *parent = nullptr);

	void set_workspace(workspace *ws);
	// Makes the editors again for the current selection.
	void rebuild();
	// Updates the live values. Makes the editors again if the model changed
	// from another place (and no editor has the focus).
	void refresh();
	// Source of the live lines for vehicles and pedestrians.
	void set_live_source(std::function<QStringList(const element_ref &, QString *)> source) { m_live_source = source; }

signals:
	void edit_light_program(int light_id);
	void remove_selected();
	void edited();
	// A link in the live data (for example the vehicle that causes a wait).
	void go_to(const element_ref &ref);

private:
	void apply(const QString &key, const QVariant &value);

	workspace *m_ws;
	element_ref m_ref;
	std::vector<int> m_roads;       // more selected roads (2 or more), else empty
	quint64 m_revision;
	QLabel *m_title;
	QScrollArea *m_scroll;
	QWidget *m_content;
	QVBoxLayout *m_layout;
	QLabel *m_live;
	QPushButton *m_program;
	QPushButton *m_remove;
	std::function<QStringList(const element_ref &, QString *)> m_live_source;
};

#endif // PROPERTY_PANEL_H
