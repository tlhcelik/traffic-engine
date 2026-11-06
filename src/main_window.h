#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "osm_import.h"
#include "world_view.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>

#include <memory>

class QAction;
class QActionGroup;
class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QTabWidget;
class QToolButton;
class api_server;
class chart_dock;
class congestion_dock;
class property_panel;
class sign_palette;
class traffic_density_widget;
class workspace;

namespace Ui
{
	class main_window;
}

// main_window
// -----------
// The main window of Traffic Engine:
//   * workspace tabs (each tab is one .te file with its own world view),
//   * the tool palette on the left (F1..F11) and the main toolbar on the top
//     (with the global traffic density button),
//   * the docks: Properties, Signs, Charts and Congestion,
//   * the status bar (mode, clock, vehicles, real speed, logging).
// A timer drives the simulation of all workspaces in real time.

class main_window : public QMainWindow
{
	Q_OBJECT

public:
	explicit main_window(QWidget *parent = nullptr);
	~main_window() override;

	// Shows the window with the geometry of the last session (config.xml).
	void show_restored();
	// Opens workspace files (*.te), each in a new tab. An OpenStreetMap file
	// (*.osm) is imported with the options of the last import.
	void open_files(const QStringList &files);
	// Imports an area from the Overpass API without the dialog (command line).
	void import_osm_area(double lat, double lon, double size_m);
	// Starts the external API on 127.0.0.1 for this session (command line --api-port).
	void start_api_session(int port);
	// Opens the start workspace (the demo or an empty world).
	void open_start_workspace();
	// Shows a message if the program crashed in an earlier session.
	void check_previous_crash();
	// Test mode (--screenshot): saves the window and the world view after
	// "delay" seconds, then closes the program. density >= 0: traffic density
	// in percent for this test only (config.xml does not change).
	void schedule_screenshot(const QString &path, double delay, double speed, bool dev, const QString &select, double zoom, double density);
	// Test mode, view of the screenshot: window size (empty size: no change)
	// and center of the view (node >= 0: this node; else "center" if valid).
	void set_shot_view(const QSize &size, int node, const QPointF &center, bool center_valid);
	// Test mode, frames for an animation: after the delay, "count" images of
	// the view, one each "interval" seconds of simulated time
	// (<file>_f000.png, <file>_f001.png, ...).
	void set_shot_frames(int count, double interval);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	world_view *current_view() const;
	workspace *current_ws() const;
	world_view *view_at(int index) const;
	world_view *add_workspace(workspace *ws);
	void new_workspace(bool demo);
	void open_dialog();
	bool save_workspace(int index, bool save_as);
	bool close_tab(int index);
	void update_tab_titles();
	void update_actions();
	void update_status();
	void current_changed();
	void tick();
	void set_tool(int tool);
	void set_speed(double factor);
	void export_json();
	void export_view_png();
	void import_osm();
	void run_osm_job(const QString &file, double lat, double lon, double size_m);
	void add_osm_workspace(const std::shared_ptr<world_model> &model, const osm_import_result &result, const QString &name);
	void edit_light_program(int light_id);
	// Selects the element and shows it in the view (links of the docks)
	void go_to_element(const element_ref &ref);
	void show_settings();
	// Language of the user interface (config general/language). apply_language()
	// translates the window, the docks and the texts of the code again (live).
	void set_language(const QString &code);
	void apply_language();
	void show_shortcuts();
	void show_about();
	void fill_recent_menu();
	void take_screenshot();
	void run_editor_script(const QString &shot_path);
	void run_levels_script();

	void build_actions();
	void build_menus();
	void build_toolbars();
	void build_docks();
	void build_status();

	Ui::main_window *ui;
	QTabWidget *m_tabs;
	QDockWidget *m_props_dock;
	QDockWidget *m_signs_dock;
	QDockWidget *m_charts_dock;
	QDockWidget *m_congestion_dock;
	property_panel *m_props;
	sign_palette *m_signs;
	chart_dock *m_charts;
	congestion_dock *m_congestion;

	QActionGroup *m_tool_group;
	QAction *m_tool_actions[world_view::tool_count];
	QAction *a_new;
	QAction *a_demo;
	QAction *a_open;
	QAction *a_save;
	QAction *a_save_as;
	QAction *a_close;
	QAction *a_export_json;
	QAction *a_export_png;
	QAction *a_import_osm;
	QAction *a_map;
	QAction *a_exit;
	QAction *a_editor;
	QAction *a_run;
	QAction *a_step;
	QAction *a_reset;
	QAction *a_faster;
	QAction *a_slower;
	QAction *a_delete;
	QAction *a_dev;
	QAction *a_countdowns;
	QAction *a_labels;
	QAction *a_grid;
	QAction *a_density;
	QAction *a_congestion;
	QAction *a_fit;
	QAction *a_follow;
	QAction *a_program;
	QAction *a_settings;
	QMenu *m_recent_menu;
	QComboBox *m_speed_box;
	QMenu *m_density_menu;              // toolbar button and Simulation menu
	QToolButton *m_density_button;
	traffic_density_widget *m_density;
	QMenu *m_lang_menu;                 // toolbar button and View menu
	QActionGroup *m_lang_group;
	QToolButton *m_lang_button;

	QLabel *m_st_mode;
	QLabel *m_st_clock;
	QLabel *m_st_vehicles;
	QLabel *m_st_speed;
	QLabel *m_st_log;
	QLabel *m_st_api;
	api_server *m_api;

	QTimer m_timer;
	QElapsedTimer m_wall;
	qint64 m_last_ns;
	QElapsedTimer m_status_timer;
	QElapsedTimer m_chart_timer;

	QString m_shot_path;
	double m_shot_delay;
	double m_shot_speed;
	double m_shot_density;
	QString m_shot_select;
	bool m_shot_selected;
	QElapsedTimer m_shot_timer;
	bool m_test_mode;           // the window state does not go to config.xml
	int m_shot_frames;
	int m_shot_frame;
	double m_shot_interval;
	double m_shot_next;         // simulated time of the next frame
};

#endif // MAIN_WINDOW_H
