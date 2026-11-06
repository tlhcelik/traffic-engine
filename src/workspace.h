#ifndef WORKSPACE_H
#define WORKSPACE_H

#include "road_network.h"
#include "simulation.h"
#include "world_model.h"
#include "world_properties.h"

#include <QObject>
#include <QString>

// workspace
// ---------
// One open workspace (one tab): the world model, the road network made from
// it, the simulation, the file path and the state of the user interface
// (editor mode, run, speed, selection).
//
// Editor mode: the flow stops. The user can add, move and remove elements.
// Run mode: the flow continues in real time x speed. The properties can
// change at runtime; the simulation uses the new values immediately.
//
// After each model change, call model_changed(). The workspace updates the
// road network (quick update or new network) and keeps the vehicles.

class workspace : public QObject
{
	Q_OBJECT

public:
	explicit workspace(QObject *parent = nullptr);
	~workspace() override;

	world_model &model() { return m_model; }
	const world_model &model() const { return m_model; }
	const road_network &network() const { return m_net; }
	simulation &sim() { return m_sim; }
	const simulation &sim() const { return m_sim; }

	QString file_path() const { return m_path; }
	void set_file_path(const QString &path);
	QString display_name() const;
	bool dirty() const { return m_dirty; }
	void set_dirty(bool on);

	bool editor_mode() const { return m_editor; }
	void set_editor_mode(bool on);
	bool running() const { return m_running; }
	void set_running(bool on);
	double speed() const { return m_speed; }
	void set_speed(double factor);
	// True if the flow moves now (run mode and not paused).
	bool flowing() const { return m_running && !m_editor; }

	// Call after each change of the model.
	void model_changed(property_change change);
	// Makes the road network again if the geometry changed.
	void sync_network();
	// Advances the simulation (real time x speed) if the flow moves.
	void tick(double wall_dt);
	// Starts the simulation again (no vehicles, clock at the start).
	void reset_simulation();
	// One step of the simulation (when paused).
	void single_step();

	// The selection: one element. With Ctrl+click more roads can be selected
	// together; the last added road is the main selection (selection()).
	element_ref selection() const { return m_selection; }
	void select(const element_ref &ref);
	// A road goes into the selection, or out of it if it is selected.
	void toggle_road(int road_id);
	// All selected roads, the main road last (empty: the selection is not a road).
	std::vector<int> selected_roads() const;
	// Removes the selection if the element does not exist anymore.
	void validate_selection();

	bool load(const QString &path, QString *error);
	bool save(const QString &path, QString *error);

	// Changes when the drawing of the static world must be made again.
	quint64 view_revision() const { return m_model.revision * 1000003ULL + m_net.built_revision(); }
	int untitled_number() const { return m_untitled; }

signals:
	void changed();
	void selection_changed(const element_ref &ref);
	void mode_changed();
	void network_rebuilt();

private:
	world_model m_model;
	road_network m_net;
	simulation m_sim;
	QString m_path;
	bool m_dirty;
	bool m_editor;
	bool m_running;
	double m_speed;
	element_ref m_selection;
	std::vector<int> m_more_roads;  // roads selected together with the main road m_selection
	int m_untitled;
};

#endif // WORKSPACE_H
