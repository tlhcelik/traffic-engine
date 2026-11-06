#include "main_window.h"
#include "ui_main_window.h"

#include "api_server.h"
#include "app_paths.h"
#include "chart_dock.h"
#include "congestion_dock.h"
#include "config_common.h"
#include "crash_handler.h"
#include "debug_tools.h"
#include "demo_world.h"
#include "i18n.h"
#include "icon_factory.h"
#include "json_export.h"
#include "logger.h"
#include "osm_area_map.h"
#include "osm_import_dialog.h"
#include "property_panel.h"
#include "settings_dialog.h"
#include "sign_palette.h"
#include "stats_collector.h"
#include "te_file.h"
#include "traffic_density_widget.h"
#include "traffic_light_dialog.h"
#include "workspace.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMenu>
#include <QPushButton>
#include <QMenuBar>
#include <QMessageBox>
#include <QSaveFile>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTime>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QWidgetAction>

#include <boost/version.hpp>
#include <xercesc/util/XercesVersion.hpp>

namespace
{
	const double speeds[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0 };

	// Order of the tools in the tool bar and in the menu: the bridge and the
	// tunnel come after the road
	const std::vector<int> &tool_order()
	{
		static const std::vector<int> order = {
			world_view::tool_select, world_view::tool_road, world_view::tool_bridge, world_view::tool_tunnel,
			world_view::tool_light, world_view::tool_sign, world_view::tool_cone, world_view::tool_barrier,
			world_view::tool_closure, world_view::tool_crossing, world_view::tool_bus_stop, world_view::tool_spawn,
			world_view::tool_erase
		};
		return order;
	}
}

main_window::main_window(QWidget *parent)
	: QMainWindow(parent)
	, ui(new Ui::main_window)
	, m_last_ns(0)
	, m_shot_delay(0.0)
	, m_shot_speed(1.0)
	, m_shot_density(-1.0)
	, m_shot_selected(false)
	, m_test_mode(false)
	, m_shot_frames(0)
	, m_shot_frame(0)
	, m_shot_interval(0.1)
	, m_shot_next(-1.0)
{
	ui->setupUi(this);
	QString title = QStringLiteral("Traffic Engine %1").arg(QStringLiteral(TE_VERSION));
	if (debug_tools::is_debug_mode())
	{
		title += tx("  [DEBUG MODE]");
	}
	setWindowTitle(title);
	setDockNestingEnabled(true);

	m_tabs = new QTabWidget(this);
	m_tabs->setTabsClosable(true);
	m_tabs->setMovable(true);
	m_tabs->setDocumentMode(true);
	setCentralWidget(m_tabs); // QMainWindow deletes the old central widget of the .ui file
	ui->central_widget = nullptr;

	build_actions();
	build_docks();
	build_menus();
	build_toolbars();
	build_status();

	connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { current_changed(); });
	connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { close_tab(i); });
	// The global traffic density can change in the popup or in the settings dialog.
	connect(&config_common::instance(), &config_common::changed, this, [this](const QString &key)
	{
		if (key == QLatin1String("simulation/traffic_density") || key == QLatin1String("simulation/pedestrian_density"))
		{
			m_density->reload();
			m_density_button->setText(traffic_density_widget::button_text());
			m_density_button->setToolTip(traffic_density_widget::button_tip());
		}
	});

	// External API (HTTP + JSON, docs/api.md): off by default (Settings > API).
	m_api = new api_server(this);
	m_api->set_sources([this]()
	{
		QList<workspace *> list;
		for (int i = 0; i < m_tabs->count(); ++i)
		{
			list.append(view_at(i)->ws());
		}
		return list;
	}, [this]() { return current_ws(); });
	connect(m_api, &api_server::state_changed, this, [this]()
	{
		m_st_api->setText(m_api->running() ? tx("API: %1").arg(m_api->address()) : QString());
		m_st_api->setVisible(m_api->running());
	});
	m_api->apply_settings();

	// Language: the tabs (workspace names) and the status labels have texts
	// from the code; the other texts come from the dictionary.
	i18n::set_dynamic(m_tabs);
	i18n::set_dynamic(m_density_button);
	for (QLabel *l : { m_st_mode, m_st_clock, m_st_vehicles, m_st_speed, m_st_log, m_st_api })
	{
		i18n::set_dynamic(l);
	}
	apply_language();

	// The timer drives the frames (frame limit, default 60 FPS; 0 = no limit).
	const int limit = config_common::instance().int_value(QStringLiteral("render/fps_limit"));
	m_timer.setTimerType(Qt::PreciseTimer);
	m_timer.setInterval(limit > 0 ? std::max(1, 1000 / limit) : 1);
	connect(&m_timer, &QTimer::timeout, this, &main_window::tick);
	m_wall.start();
	m_status_timer.start();
	m_chart_timer.start();
	m_timer.start();
}

main_window::~main_window()
{
	m_timer.stop();
	delete ui;
}

// ---------------------------------------------------------------------------
// building the user interface
// ---------------------------------------------------------------------------
void main_window::build_actions()
{
	auto make = [this](const QString &text, const QString &icon, const QKeySequence &key, const QString &tip)
	{
		QAction *a = new QAction(icon.isEmpty() ? QIcon() : icon_factory::icon(icon), text, this);
		if (!key.isEmpty())
		{
			a->setShortcut(key);
		}
		a->setStatusTip(tip);
		a->setToolTip(key.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, key.toString(QKeySequence::NativeText)));
		return a;
	};

	a_new = make(QStringLiteral("&New workspace"), QStringLiteral("new"), QKeySequence::New, QStringLiteral("Make a new empty workspace"));
	a_demo = make(QStringLiteral("New &demo workspace"), QStringLiteral("demo"), QKeySequence(QStringLiteral("Ctrl+Shift+N")), QStringLiteral("Make a workspace with the demo city"));
	a_open = make(QStringLiteral("&Open..."), QStringLiteral("open"), QKeySequence::Open, QStringLiteral("Open workspace files (*.te)"));
	a_save = make(QStringLiteral("&Save"), QStringLiteral("save"), QKeySequence::Save, QStringLiteral("Save the workspace"));
	a_save_as = make(QStringLiteral("Save &as..."), QString(), QKeySequence(QStringLiteral("Ctrl+Shift+S")), QStringLiteral("Save the workspace with a new name"));
	a_close = make(QStringLiteral("&Close workspace"), QString(), QKeySequence(QStringLiteral("Ctrl+W")), QStringLiteral("Close the current workspace"));
	a_export_json = make(QStringLiteral("Export the report (&JSON)..."), QStringLiteral("json"), QKeySequence(QStringLiteral("Ctrl+J")), QStringLiteral("Write the program and workspace data as JSON"));
	a_export_png = make(QStringLiteral("Export the &view (PNG)..."), QStringLiteral("screenshot"), QKeySequence(), QStringLiteral("Save the world view as an image"));
	a_import_osm = make(QStringLiteral("&Import from OpenStreetMap..."), QStringLiteral("osm"), QKeySequence(QStringLiteral("Ctrl+I")),
		QStringLiteral("Make a workspace from the roads of a real area (OpenStreetMap)"));
	a_exit = make(QStringLiteral("E&xit"), QString(), QKeySequence::Quit, QStringLiteral("Close the program"));

	a_editor = make(QStringLiteral("&Editor mode"), QStringLiteral("editor"), QKeySequence(QStringLiteral("Ctrl+E")), QStringLiteral("Editor mode: the flow stops and you can edit the world"));
	a_editor->setCheckable(true);
	a_run = make(QStringLiteral("&Run / pause"), QStringLiteral("run"), QKeySequence(Qt::Key_Space), QStringLiteral("Run or pause the flow"));
	a_step = make(QStringLiteral("One &step"), QStringLiteral("step"), QKeySequence(Qt::Key_Period), QStringLiteral("One simulation step (when paused)"));
	a_reset = make(QStringLiteral("Start &again"), QStringLiteral("reset"), QKeySequence(QStringLiteral("Ctrl+R")), QStringLiteral("Start the simulation again (no vehicles, clock at the start)"));
	a_faster = make(QStringLiteral("&Faster"), QStringLiteral("faster"), QKeySequence(Qt::Key_Plus), QStringLiteral("Double the speed of the flow"));
	a_slower = make(QStringLiteral("S&lower"), QStringLiteral("slower"), QKeySequence(Qt::Key_Minus), QStringLiteral("Half the speed of the flow"));
	a_delete = make(QStringLiteral("&Remove the selection"), QStringLiteral("delete"), QKeySequence::Delete, QStringLiteral("Remove the selected element (editor mode)"));

	a_dev = make(QStringLiteral("&Developer details"), QStringLiteral("dev"), QKeySequence(Qt::Key_F12), QStringLiteral("Show FPS, timings and other developer data"));
	a_dev->setCheckable(true);
	a_countdowns = make(QStringLiteral("Traffic light &countdowns"), QString(), QKeySequence(), QStringLiteral("Show the remaining time of the traffic lights in the world"));
	a_countdowns->setCheckable(true);
	a_labels = make(QStringLiteral("Road &names"), QString(), QKeySequence(), QStringLiteral("Show the road names"));
	a_labels->setCheckable(true);
	a_grid = make(QStringLiteral("&Grid"), QString(), QKeySequence(QStringLiteral("Ctrl+G")), QStringLiteral("Show a grid (10 m and 100 m)"));
	a_grid->setCheckable(true);
	a_density = make(QStringLiteral("Traffic &density map"), QString(), QKeySequence(), QStringLiteral("Show the traffic density by colors when you zoom out"));
	a_density->setCheckable(true);
	a_density->setChecked(true);
	a_congestion = make(QStringLiteral("&Congestion analysis"), QStringLiteral("congestion"), QKeySequence(QStringLiteral("Ctrl+K")),
		QStringLiteral("Show the congested junctions: level of service, queues and the causes of the waits"));
	a_congestion->setCheckable(true);
	a_map = make(QStringLiteral("&Map background"), QStringLiteral("map"), QKeySequence(QStringLiteral("Ctrl+M")),
		QStringLiteral("Show the OpenStreetMap map under a workspace with a geographic origin"));
	a_map->setCheckable(true);
	a_fit = make(QStringLiteral("&Zoom to fit"), QStringLiteral("fit"), QKeySequence(QStringLiteral("Ctrl+0")), QStringLiteral("Show the full world"));
	a_follow = make(QStringLiteral("&Follow the selected vehicle"), QStringLiteral("follow"), QKeySequence(QStringLiteral("Ctrl+F")), QStringLiteral("The view follows the selected vehicle or pedestrian"));
	a_follow->setCheckable(true);
	a_program = make(QStringLiteral("Traffic light &program..."), QStringLiteral("program"), QKeySequence(QStringLiteral("Ctrl+L")), QStringLiteral("Edit the phases and the times of the selected traffic light"));
	a_settings = make(QStringLiteral("&Settings..."), QStringLiteral("settings"), QKeySequence(QStringLiteral("Ctrl+,")), QStringLiteral("Settings of the program"));

	const config_common &cfg = config_common::instance();
	a_dev->setChecked(cfg.bool_value(QStringLiteral("render/show_dev_overlay")) || debug_tools::is_debug_mode());
	a_countdowns->setChecked(cfg.bool_value(QStringLiteral("render/show_countdowns")));
	a_labels->setChecked(cfg.bool_value(QStringLiteral("render/show_labels")));
	a_grid->setChecked(cfg.bool_value(QStringLiteral("render/show_grid")));
	a_map->setChecked(cfg.bool_value(QStringLiteral("render/show_map")));

	// Keys that a text field also uses: active only when the world view has the focus.
	for (QAction *a : { a_run, a_step, a_faster, a_slower, a_delete })
	{
		a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
		m_tabs->addAction(a);
	}

	m_tool_group = new QActionGroup(this);
	m_tool_group->setExclusive(true);
	for (int t = 0; t < world_view::tool_count; ++t)
	{
		// F1 .. F11; the bridge and tunnel tools: Shift+F2 and Ctrl+F2 (near the road tool, F2)
		QKeySequence key(Qt::Key_F1 + t);
		if (t == world_view::tool_bridge)
		{
			key = QKeySequence(QStringLiteral("Shift+F2"));
		}
		else if (t == world_view::tool_tunnel)
		{
			key = QKeySequence(QStringLiteral("Ctrl+F2"));
		}
		QAction *a = make(world_view::tool_name(t), world_view::tool_icon(t), key, world_view::tool_hint(t));
		a->setCheckable(true);
		a->setData(t);
		m_tool_group->addAction(a);
		m_tool_actions[t] = a;
		connect(a, &QAction::triggered, this, [this, t]() { set_tool(t); });
	}
	m_tool_actions[0]->setChecked(true);

	connect(a_new, &QAction::triggered, this, [this]() { new_workspace(false); });
	connect(a_demo, &QAction::triggered, this, [this]() { new_workspace(true); });
	connect(a_open, &QAction::triggered, this, &main_window::open_dialog);
	connect(a_save, &QAction::triggered, this, [this]() { save_workspace(m_tabs->currentIndex(), false); });
	connect(a_save_as, &QAction::triggered, this, [this]() { save_workspace(m_tabs->currentIndex(), true); });
	connect(a_close, &QAction::triggered, this, [this]() { close_tab(m_tabs->currentIndex()); });
	connect(a_export_json, &QAction::triggered, this, &main_window::export_json);
	connect(a_export_png, &QAction::triggered, this, &main_window::export_view_png);
	connect(a_import_osm, &QAction::triggered, this, &main_window::import_osm);
	connect(a_exit, &QAction::triggered, this, &QWidget::close);
	connect(a_editor, &QAction::triggered, this, [this](bool on)
	{
		if (workspace *ws = current_ws())
		{
			ws->set_editor_mode(on);
		}
	});
	connect(a_run, &QAction::triggered, this, [this]()
	{
		workspace *ws = current_ws();
		if (!ws)
		{
			return;
		}
		if (ws->editor_mode())
		{
			ws->set_editor_mode(false);
			ws->set_running(true);
		}
		else
		{
			ws->set_running(!ws->running());
		}
	});
	connect(a_step, &QAction::triggered, this, [this]()
	{
		workspace *ws = current_ws();
		if (ws && !ws->flowing())
		{
			ws->single_step();
		}
	});
	connect(a_reset, &QAction::triggered, this, [this]()
	{
		if (workspace *ws = current_ws())
		{
			ws->reset_simulation();
			logger::instance().action(QStringLiteral("%1: simulation starts again").arg(ws->display_name()));
		}
	});
	connect(a_faster, &QAction::triggered, this, [this]() { if (current_ws()) { set_speed(current_ws()->speed() * 2.0); } });
	connect(a_slower, &QAction::triggered, this, [this]() { if (current_ws()) { set_speed(current_ws()->speed() / 2.0); } });
	connect(a_delete, &QAction::triggered, this, [this]()
	{
		if (world_view *v = current_view())
		{
			QKeyEvent ev(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
			QApplication::sendEvent(v, &ev);
		}
	});
	connect(a_dev, &QAction::toggled, this, [this](bool on)
	{
		for (int i = 0; i < m_tabs->count(); ++i)
		{
			view_at(i)->set_dev_overlay(on);
		}
	});
	connect(a_countdowns, &QAction::toggled, this, [this](bool on) { for (int i = 0; i < m_tabs->count(); ++i) { view_at(i)->set_show_countdowns(on); } });
	connect(a_labels, &QAction::toggled, this, [this](bool on) { for (int i = 0; i < m_tabs->count(); ++i) { view_at(i)->set_show_labels(on); } });
	connect(a_grid, &QAction::toggled, this, [this](bool on) { for (int i = 0; i < m_tabs->count(); ++i) { view_at(i)->set_show_grid(on); } });
	connect(a_density, &QAction::toggled, this, [this](bool on) { for (int i = 0; i < m_tabs->count(); ++i) { view_at(i)->set_show_density(on); } });
	connect(a_congestion, &QAction::toggled, this, [this](bool on)
	{
		for (int i = 0; i < m_tabs->count(); ++i)
		{
			view_at(i)->set_show_congestion(on);
		}
		if (on)
		{
			m_congestion_dock->show();
			m_congestion_dock->raise();
			m_congestion->refresh();
		}
		logger::instance().action(on ? QStringLiteral("Congestion analysis on") : QStringLiteral("Congestion analysis off"));
	});
	connect(a_map, &QAction::toggled, this, [this](bool on)
	{
		for (int i = 0; i < m_tabs->count(); ++i)
		{
			view_at(i)->set_show_map(on);
		}
		config_common::instance().set_bool(QStringLiteral("render/show_map"), on);
	});
	connect(a_fit, &QAction::triggered, this, [this]() { if (current_view()) { current_view()->zoom_to_fit(); } });
	connect(a_follow, &QAction::toggled, this, [this](bool on) { if (current_view()) { current_view()->set_follow(on); } });
	connect(a_program, &QAction::triggered, this, [this]()
	{
		workspace *ws = current_ws();
		if (!ws)
		{
			return;
		}
		const element_ref sel = ws->selection();
		int lid = -1;
		if (sel.kind == element_kind::light)
		{
			lid = sel.id;
		}
		else if (sel.kind == element_kind::node)
		{
			lid = ws->model().light_at_node(sel.id);
		}
		if (lid < 0)
		{
			QMessageBox::information(this, tx("Traffic light program"),
				tx("Select a traffic light first (click a signal head or the junction)."));
			return;
		}
		edit_light_program(lid);
	});
	connect(a_settings, &QAction::triggered, this, &main_window::show_settings);

	// Global traffic density: the same menu is in the toolbar and in the Simulation menu.
	m_density = new traffic_density_widget();
	m_density_menu = new QMenu(QStringLiteral("Traffic &density"), this);
	m_density_menu->setIcon(icon_factory::icon(QStringLiteral("density")));
	QWidgetAction *density_action = new QWidgetAction(m_density_menu);
	density_action->setDefaultWidget(m_density); // the action owns the widget
	m_density_menu->addAction(density_action);

	// Language of the user interface: the names stay in their own language.
	m_lang_menu = new QMenu(QStringLiteral("&Language"), this);
	m_lang_menu->setIcon(icon_factory::icon(QStringLiteral("language")));
	m_lang_group = new QActionGroup(this);
	m_lang_group->setExclusive(true);
	for (const QPair<QString, QString> &lang : i18n::languages())
	{
		QAction *a = m_lang_menu->addAction(lang.second);
		a->setCheckable(true);
		a->setData(lang.first);
		i18n::set_dynamic(a);
		m_lang_group->addAction(a);
		const QString code = lang.first;
		connect(a, &QAction::triggered, this, [this, code]() { set_language(code); });
	}
}

void main_window::set_language(const QString &code)
{
	if (code == i18n::language())
	{
		return;
	}
	i18n::set_language(code);
	config_common &cfg = config_common::instance();
	cfg.set_value(QStringLiteral("general/language"), i18n::language());
	cfg.save();
	logger::instance().action(QStringLiteral("Language: %1").arg(i18n::language()));
	apply_language();
}

void main_window::apply_language()
{
	// Fixed texts of the window, the docks and the popup of the density button
	i18n::retranslate(this);
	i18n::retranslate(m_density);
	// Texts that the code makes
	m_density_button->setText(traffic_density_widget::button_text());
	m_density_button->setToolTip(traffic_density_widget::button_tip());
	m_lang_button->setText(i18n::language().toUpper());
	m_lang_button->setToolTip(tx("Language of the user interface"));
	m_speed_box->setToolTip(tx("Speed of the flow (real time = x1)"));
	for (QAction *a : m_lang_group->actions())
	{
		a->setChecked(a->data().toString() == i18n::language());
	}
	m_props->rebuild();
	m_signs->reload();
	m_charts->retranslate();
	m_congestion->retranslate();
	update_tab_titles();
	update_actions();
	update_status();
	for (int i = 0; i < m_tabs->count(); ++i)
	{
		view_at(i)->update();
	}
}

void main_window::build_menus()
{
	QMenuBar *mb = ui->menu_bar;
	QMenu *file = mb->addMenu(QStringLiteral("&File"));
	file->addAction(a_new);
	file->addAction(a_demo);
	file->addAction(a_open);
	m_recent_menu = file->addMenu(QStringLiteral("Open &recent"));
	connect(m_recent_menu, &QMenu::aboutToShow, this, &main_window::fill_recent_menu);
	file->addAction(a_import_osm);
	file->addSeparator();
	file->addAction(a_save);
	file->addAction(a_save_as);
	file->addAction(a_close);
	file->addSeparator();
	file->addAction(a_export_json);
	file->addAction(a_export_png);
	file->addSeparator();
	file->addAction(a_exit);

	QMenu *edit = mb->addMenu(QStringLiteral("&Edit"));
	edit->addAction(a_editor);
	edit->addAction(a_delete);
	edit->addSeparator();
	QMenu *tools = edit->addMenu(QStringLiteral("&Tools"));
	for (int t : tool_order())
	{
		tools->addAction(m_tool_actions[t]);
	}
	QMenu *lanes = edit->addMenu(QStringLiteral("&Lanes of new roads"));
	QActionGroup *lane_group = new QActionGroup(this);
	struct lane_choice
	{
		const char *text;
		int fwd;
		int bwd;
	};
	const lane_choice choices[] =
	{
		{ "1 + 1 lanes (two-way)", 1, 1 },
		{ "2 + 2 lanes (two-way)", 2, 2 },
		{ "3 + 3 lanes (two-way)", 3, 3 },
		{ "1 lane (one-way)", 1, 0 },
		{ "2 lanes (one-way)", 2, 0 },
		{ "3 lanes (one-way)", 3, 0 }
	};
	for (const lane_choice &c : choices)
	{
		QAction *a = lanes->addAction(QString::fromLatin1(c.text));
		a->setCheckable(true);
		lane_group->addAction(a);
		const int fwd = c.fwd;
		const int bwd = c.bwd;
		a->setChecked(fwd == 1 && bwd == 1);
		connect(a, &QAction::triggered, this, [fwd, bwd]()
		{
			world_view::road_template().lanes_forward = fwd;
			world_view::road_template().lanes_backward = bwd;
			logger::instance().action(QStringLiteral("New roads: %1 + %2 lanes").arg(fwd).arg(bwd));
		});
	}
	QMenu *limit = edit->addMenu(QStringLiteral("Speed limit of new &roads"));
	QActionGroup *limit_group = new QActionGroup(this);
	for (int v : { 30, 50, 70, 90, 110 })
	{
		QAction *a = limit->addAction(QStringLiteral("%1 km/h").arg(v));
		a->setCheckable(true);
		a->setChecked(v == 50);
		limit_group->addAction(a);
		connect(a, &QAction::triggered, this, [v]() { world_view::road_template().speed_limit = v; });
	}

	QMenu *view = mb->addMenu(QStringLiteral("&View"));
	view->addAction(a_dev);
	view->addAction(a_countdowns);
	view->addAction(a_labels);
	view->addAction(a_grid);
	view->addAction(a_density);
	view->addAction(a_congestion);
	view->addAction(a_map);
	view->addSeparator();
	view->addMenu(m_lang_menu);
	QMenu *ground = view->addMenu(QStringLiteral("Ground &material"));
	const QStringList names = { QStringLiteral("Grass"), QStringLiteral("Soil"), QStringLiteral("Sand"), QStringLiteral("Snow"), QStringLiteral("Concrete"), QStringLiteral("Asphalt") };
	for (int i = 0; i < names.size(); ++i)
	{
		QAction *a = ground->addAction(names.at(i));
		connect(a, &QAction::triggered, this, [this, i]()
		{
			if (workspace *ws = current_ws())
			{
				ws->model().ground = static_cast<ground_material>(i);
				ws->model_changed(property_change::change_none);
				logger::instance().action(QStringLiteral("Ground material: %1").arg(ground_name(ws->model().ground)));
			}
		});
	}
	view->addSeparator();
	view->addAction(a_fit);
	view->addAction(a_follow);
	view->addSeparator();
	view->addAction(m_props_dock->toggleViewAction());
	view->addAction(m_signs_dock->toggleViewAction());
	view->addAction(m_charts_dock->toggleViewAction());
	view->addAction(m_congestion_dock->toggleViewAction());

	QMenu *sim = mb->addMenu(QStringLiteral("&Simulation"));
	sim->addAction(a_run);
	sim->addAction(a_step);
	sim->addAction(a_reset);
	sim->addSeparator();
	sim->addAction(a_faster);
	sim->addAction(a_slower);
	sim->addSeparator();
	sim->addMenu(m_density_menu);

	QMenu *toolsm = mb->addMenu(QStringLiteral("&Tools"));
	toolsm->addAction(a_program);
	toolsm->addAction(a_settings);

	QMenu *help = mb->addMenu(QStringLiteral("&Help"));
	help->addAction(QStringLiteral("&Keys and mouse"), this, &main_window::show_shortcuts);
	help->addAction(QStringLiteral("Open the &log folder"), this, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(app_paths::log_dir())); });
	help->addAction(QStringLiteral("Open the &crash folder"), this, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(app_paths::crash_dir())); });
	help->addAction(QStringLiteral("Open the &data folder"), this, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(app_paths::data_dir())); });
	if (debug_tools::is_debug_mode())
	{
		help->addSeparator();
		help->addAction(QStringLiteral("Make a &test crash"), this, [this]()
		{
			if (QMessageBox::question(this, tx("Test crash"),
				tx("The program stops now and writes a crash report and a dump file. Do you want to continue?")) == QMessageBox::Yes)
			{
				crash_handler::force_test_crash();
			}
		});
	}
	help->addSeparator();
	help->addAction(QStringLiteral("&About Traffic Engine"), this, &main_window::show_about);
}

void main_window::build_toolbars()
{
	QToolBar *tb = addToolBar(QStringLiteral("Main toolbar"));
	tb->setObjectName(QStringLiteral("main_toolbar"));
	tb->setIconSize(QSize(28, 28));
	tb->setMovable(false);
	tb->addAction(a_new);
	tb->addAction(a_open);
	tb->addAction(a_save);
	tb->addAction(a_import_osm);
	tb->addSeparator();
	tb->addAction(a_editor);
	tb->addAction(a_run);
	tb->addAction(a_step);
	tb->addAction(a_reset);
	tb->addSeparator();
	tb->addAction(a_slower);
	m_speed_box = new QComboBox(tb);
	for (double s : speeds)
	{
		m_speed_box->addItem(QStringLiteral("x%1").arg(s), s);
	}
	i18n::set_dynamic(m_speed_box); // numbers; apply_language() sets the tool tip
	tb->addWidget(m_speed_box);
	connect(m_speed_box, QOverload<int>::of(&QComboBox::activated), this, [this](int i) { set_speed(m_speed_box->itemData(i).toDouble()); });
	tb->addAction(a_faster);
	tb->addSeparator();
	m_density_button = new QToolButton(tb);
	m_density_button->setMenu(m_density_menu);
	m_density_button->setPopupMode(QToolButton::InstantPopup);
	m_density_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	m_density_button->setIcon(icon_factory::icon(QStringLiteral("density")));
	m_density_button->setText(traffic_density_widget::button_text());
	m_density_button->setToolTip(traffic_density_widget::button_tip());
	tb->addWidget(m_density_button);
	tb->addSeparator();
	tb->addAction(a_fit);
	tb->addAction(a_follow);
	tb->addAction(a_map);
	tb->addAction(m_charts_dock->toggleViewAction());
	m_charts_dock->toggleViewAction()->setIcon(icon_factory::icon(QStringLiteral("charts")));
	tb->addAction(a_congestion);
	tb->addAction(a_program);
	tb->addAction(a_dev);
	tb->addAction(a_settings);
	m_lang_button = new QToolButton(tb);
	m_lang_button->setMenu(m_lang_menu);
	m_lang_button->setPopupMode(QToolButton::InstantPopup);
	m_lang_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	m_lang_button->setIcon(icon_factory::icon(QStringLiteral("language")));
	i18n::set_dynamic(m_lang_button);
	tb->addWidget(m_lang_button);

	QToolBar *palette = new QToolBar(QStringLiteral("Tools"), this);
	palette->setObjectName(QStringLiteral("tool_palette"));
	palette->setIconSize(QSize(34, 34));
	palette->setMovable(false);
	palette->setOrientation(Qt::Vertical);
	for (int t : tool_order())
	{
		palette->addAction(m_tool_actions[t]);
	}
	addToolBar(Qt::LeftToolBarArea, palette);
}

void main_window::build_docks()
{
	m_props = new property_panel(this);
	m_props_dock = new QDockWidget(QStringLiteral("Properties"), this);
	m_props_dock->setObjectName(QStringLiteral("properties_dock"));
	m_props_dock->setWidget(m_props);
	m_props_dock->toggleViewAction()->setIcon(icon_factory::icon(QStringLiteral("properties")));
	addDockWidget(Qt::RightDockWidgetArea, m_props_dock);

	m_signs = new sign_palette(this);
	m_signs_dock = new QDockWidget(QStringLiteral("Signs"), this);
	m_signs_dock->setObjectName(QStringLiteral("signs_dock"));
	m_signs_dock->setWidget(m_signs);
	m_signs_dock->toggleViewAction()->setIcon(icon_factory::icon(QStringLiteral("signs")));
	addDockWidget(Qt::RightDockWidgetArea, m_signs_dock);
	tabifyDockWidget(m_props_dock, m_signs_dock);
	m_props_dock->raise();

	m_charts = new chart_dock(this);
	m_charts_dock = new QDockWidget(QStringLiteral("Charts"), this);
	m_charts_dock->setObjectName(QStringLiteral("charts_dock"));
	m_charts_dock->setWidget(m_charts);
	addDockWidget(Qt::BottomDockWidgetArea, m_charts_dock);
	m_charts_dock->hide();

	m_congestion = new congestion_dock(this);
	m_congestion_dock = new QDockWidget(QStringLiteral("Congestion"), this);
	m_congestion_dock->setObjectName(QStringLiteral("congestion_dock"));
	m_congestion_dock->setWidget(m_congestion);
	addDockWidget(Qt::BottomDockWidgetArea, m_congestion_dock);
	tabifyDockWidget(m_charts_dock, m_congestion_dock);
	m_congestion_dock->hide();
	connect(m_congestion, &congestion_dock::go_to, this, &main_window::go_to_element);

	connect(m_props, &property_panel::edit_light_program, this, &main_window::edit_light_program);
	connect(m_props, &property_panel::remove_selected, this, [this]()
	{
		if (world_view *v = current_view())
		{
			QKeyEvent ev(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
			QApplication::sendEvent(v, &ev);
		}
	});
	connect(m_props, &property_panel::edited, this, [this]() { update_tab_titles(); });
	m_props->set_live_source([this](const element_ref &ref, QString *title) -> QStringList
	{
		world_view *v = current_view();
		vec2 anchor;
		return v ? v->popup_lines(ref, &anchor, title, true) : QStringList();
	});
	// A link in the live data: select the element and show it in the view.
	connect(m_props, &property_panel::go_to, this, &main_window::go_to_element);
	connect(m_signs, &sign_palette::sign_chosen, this, [this](const QString &key)
	{
		for (int i = 0; i < m_tabs->count(); ++i)
		{
			view_at(i)->set_sign_key(key);
		}
		set_tool(world_view::tool_sign);
		statusBar()->showMessage(tx("Sign tool: click a road side to put the sign there."), 6000);
	});
}

void main_window::go_to_element(const element_ref &ref)
{
	world_view *v = current_view();
	workspace *ws = current_ws();
	if (!v || !ws)
	{
		return;
	}
	vec2 anchor;
	QString title;
	if (ref.kind == element_kind::road)
	{
		// A road: its middle point (the popup lines of a road have no anchor here)
		const road_data *r = ws->model().road(ref.id);
		if (!r)
		{
			return;
		}
		const polyline c = ws->model().road_centerline(*r);
		anchor = c.point_at(c.length() / 2.0);
	}
	else if (v->popup_lines(ref, &anchor, &title).isEmpty())
	{
		statusBar()->showMessage(tx("The element does not exist anymore."), 4000);
		return;
	}
	logger::instance().action(QStringLiteral("Go to %1").arg(world_properties::title(ws->model(), ref, false)));
	ws->select(ref);
	v->center_on(anchor);
}

void main_window::build_status()
{
	auto label = [this]()
	{
		QLabel *l = new QLabel(this);
		l->setContentsMargins(8, 0, 8, 0);
		statusBar()->addPermanentWidget(l);
		return l;
	};
	m_st_mode = label();
	m_st_clock = label();
	m_st_vehicles = label();
	m_st_speed = label();
	m_st_log = label();
	m_st_api = label();
	m_st_api->hide();
	connect(&logger::instance(), &logger::guard_triggered, this, [this](int)
	{
		statusBar()->showMessage(tx("Logging slowed the program. The log level is now WARN (Settings > Logging)."), 10000);
	});
}

// ---------------------------------------------------------------------------
// workspaces
// ---------------------------------------------------------------------------
world_view *main_window::view_at(int index) const
{
	return qobject_cast<world_view *>(m_tabs->widget(index));
}

world_view *main_window::current_view() const
{
	return view_at(m_tabs->currentIndex());
}

workspace *main_window::current_ws() const
{
	world_view *v = current_view();
	return v ? v->ws() : nullptr;
}

world_view *main_window::add_workspace(workspace *ws)
{
	world_view *v = new world_view(ws, m_tabs);
	ws->setParent(v);
	if (!m_shot_path.isEmpty())
	{
		// Test mode: the speed and the density of the test also for the new workspaces
		ws->set_speed(m_shot_speed);
		if (m_shot_density >= 0.0)
		{
			ws->sim().set_demand_factors(m_shot_density / 100.0, m_shot_density / 100.0);
		}
	}
	v->set_dev_overlay(a_dev->isChecked());
	v->set_show_countdowns(a_countdowns->isChecked());
	v->set_show_labels(a_labels->isChecked());
	v->set_show_grid(a_grid->isChecked());
	v->set_show_density(a_density->isChecked());
	v->set_show_congestion(a_congestion->isChecked());
	v->set_show_map(a_map->isChecked());
	const int idx = m_tabs->addTab(v, icon_factory::icon(ws->model().has_geo() ? QStringLiteral("osm") : QStringLiteral("road")), ws->display_name());
	connect(ws, &workspace::changed, this, [this]() { update_tab_titles(); });
	connect(ws, &workspace::mode_changed, this, [this]() { update_actions(); update_status(); });
	connect(ws, &workspace::selection_changed, this, [this, ws](const element_ref &)
	{
		if (ws == current_ws())
		{
			m_props->rebuild();
			update_actions();
		}
	});
	connect(v, &world_view::status_message, this, [this](const QString &text) { statusBar()->showMessage(text, 8000); });
	connect(v, &world_view::tool_changed, this, [this](int t)
	{
		if (t >= 0 && t < world_view::tool_count)
		{
			m_tool_actions[t]->setChecked(true);
		}
	});
	connect(v, &world_view::model_edited, this, [this]() { m_props->refresh(); update_tab_titles(); });
	m_tabs->setCurrentIndex(idx);
	v->setFocus();
	update_tab_titles();
	return v;
}

void main_window::new_workspace(bool demo)
{
	workspace *ws = new workspace();
	if (demo)
	{
		demo_world::build(ws->model());
		ws->model_changed(property_change::change_network);
		ws->reset_simulation();
		ws->set_dirty(false);
	}
	add_workspace(ws);
	logger::instance().action(demo ? QStringLiteral("New demo workspace") : QStringLiteral("New empty workspace"));
	if (!demo)
	{
		ws->set_editor_mode(true);
		set_tool(world_view::tool_road);
		statusBar()->showMessage(tx("Empty workspace: draw roads with the road tool (F2). Dead ends make traffic."), 10000);
	}
}

void main_window::open_start_workspace()
{
	if (m_tabs->count() > 0)
	{
		return;
	}
	new_workspace(config_common::instance().bool_value(QStringLiteral("general/open_demo_on_start")));
}

void main_window::open_files(const QStringList &files)
{
	config_common &cfg = config_common::instance();
	for (const QString &f : files)
	{
		const QString abs = QFileInfo(f).absoluteFilePath();
		if (QFileInfo(abs).suffix().compare(QStringLiteral("osm"), Qt::CaseInsensitive) == 0)
		{
			run_osm_job(abs, 0.0, 0.0, 0.0);
			continue;
		}
		bool open = false;
		for (int i = 0; i < m_tabs->count(); ++i)
		{
			if (QFileInfo(view_at(i)->ws()->file_path()) == QFileInfo(abs))
			{
				m_tabs->setCurrentIndex(i);
				open = true;
				break;
			}
		}
		if (open)
		{
			continue;
		}
		workspace *ws = new workspace();
		QString error;
		if (!ws->load(abs, &error))
		{
			delete ws;
			TE_ERROR("FILE", QStringLiteral("Cannot open %1: %2").arg(abs, error));
			QMessageBox::warning(this, tx("Open"), tx("Cannot open the file:\n%1\n\n%2").arg(abs, i18n::tx_message(error)));
			cfg.remove_recent_file(abs);
			continue;
		}
		add_workspace(ws);
		cfg.add_recent_file(abs);
		cfg.set_value(QStringLiteral("general/last_dir"), QFileInfo(abs).absolutePath());
	}
	cfg.save();
	if (m_tabs->count() == 0)
	{
		open_start_workspace();
	}
}

void main_window::open_dialog()
{
	const QStringList files = QFileDialog::getOpenFileNames(this, tx("Open workspace files"),
		config_common::instance().value(QStringLiteral("general/last_dir")),
		tx("Traffic Engine workspace (*.te);;OpenStreetMap data (*.osm)"));
	if (!files.isEmpty())
	{
		open_files(files);
	}
}

bool main_window::save_workspace(int index, bool save_as)
{
	world_view *v = view_at(index);
	if (!v)
	{
		return false;
	}
	workspace *ws = v->ws();
	config_common &cfg = config_common::instance();
	QString path = ws->file_path();
	if (save_as || path.isEmpty())
	{
		const QString start = (path.isEmpty() ? cfg.value(QStringLiteral("general/last_dir")) + QLatin1Char('/') + ws->display_name() : path);
		path = QFileDialog::getSaveFileName(this, tx("Save the workspace"), start, tx("Traffic Engine workspace (*.te)"));
		if (path.isEmpty())
		{
			return false;
		}
		if (QFileInfo(path).suffix().isEmpty())
		{
			path += QStringLiteral(".te");
		}
	}
	QString error;
	if (!ws->save(path, &error))
	{
		QMessageBox::warning(this, tx("Save"), tx("Cannot save the file:\n%1\n\n%2").arg(path, i18n::tx_message(error)));
		return false;
	}
	cfg.add_recent_file(path);
	cfg.set_value(QStringLiteral("general/last_dir"), QFileInfo(path).absolutePath());
	cfg.save();
	statusBar()->showMessage(tx("Saved: %1").arg(path), 6000);
	update_tab_titles();
	return true;
}

bool main_window::close_tab(int index)
{
	world_view *v = view_at(index);
	if (!v)
	{
		return true;
	}
	workspace *ws = v->ws();
	if (ws->dirty())
	{
		m_tabs->setCurrentIndex(index);
		const auto answer = QMessageBox::question(this, tx("Close"),
			tx("The workspace \"%1\" has changes that are not saved. Do you want to save them?").arg(ws->display_name()),
			QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
		if (answer == QMessageBox::Cancel)
		{
			return false;
		}
		if (answer == QMessageBox::Save && !save_workspace(index, false))
		{
			return false;
		}
	}
	logger::instance().action(QStringLiteral("Workspace closed: %1").arg(ws->display_name()));
	m_tabs->removeTab(index);
	v->deleteLater();
	if (m_tabs->count() == 0)
	{
		m_props->set_workspace(nullptr);
		m_charts->set_workspace(nullptr);
		m_congestion->set_workspace(nullptr);
	}
	return true;
}

void main_window::fill_recent_menu()
{
	m_recent_menu->clear();
	const QStringList files = config_common::instance().recent_files();
	if (files.isEmpty())
	{
		m_recent_menu->addAction(tx("(no files)"))->setEnabled(false);
		return;
	}
	for (const QString &f : files)
	{
		m_recent_menu->addAction(f, this, [this, f]() { open_files({ f }); });
	}
}

void main_window::current_changed()
{
	workspace *ws = current_ws();
	m_props->set_workspace(ws);
	m_charts->set_workspace(ws);
	m_congestion->set_workspace(ws);
	if (world_view *v = current_view())
	{
		const int t = v->tool();
		m_tool_actions[t]->setChecked(true);
		a_follow->setChecked(v->follow());
	}
	update_actions();
	update_status();
}

void main_window::update_tab_titles()
{
	for (int i = 0; i < m_tabs->count(); ++i)
	{
		workspace *ws = view_at(i)->ws();
		m_tabs->setTabText(i, ws->display_name() + (ws->dirty() ? QStringLiteral(" *") : QString()));
		m_tabs->setTabToolTip(i, ws->file_path().isEmpty() ? tx("Not saved yet") : ws->file_path());
	}
	workspace *ws = current_ws();
	QString title = QStringLiteral("Traffic Engine %1").arg(QStringLiteral(TE_VERSION));
	if (ws)
	{
		title = QStringLiteral("%1%2 - %3").arg(ws->display_name(), ws->dirty() ? QStringLiteral(" *") : QString(), title);
	}
	if (debug_tools::is_debug_mode())
	{
		title += tx("  [DEBUG MODE]");
	}
	setWindowTitle(title);
}

void main_window::update_actions()
{
	workspace *ws = current_ws();
	const bool has = ws != nullptr;
	for (QAction *a : { a_save, a_save_as, a_close, a_export_json, a_export_png, a_editor, a_run, a_step, a_reset, a_faster, a_slower, a_fit, a_program })
	{
		a->setEnabled(has);
	}
	if (!ws)
	{
		return;
	}
	a_editor->setChecked(ws->editor_mode());
	const bool flowing = ws->flowing();
	a_run->setIcon(icon_factory::icon(flowing ? QStringLiteral("pause") : QStringLiteral("run")));
	a_run->setText(flowing ? tx("&Pause") : tx("&Run"));
	a_step->setEnabled(!flowing);
	a_delete->setEnabled(ws->editor_mode() && ws->selection().valid());
	int best = 0;
	for (int i = 0; i < m_speed_box->count(); ++i)
	{
		if (std::abs(m_speed_box->itemData(i).toDouble() - ws->speed()) < std::abs(m_speed_box->itemData(best).toDouble() - ws->speed()))
		{
			best = i;
		}
	}
	m_speed_box->setCurrentIndex(best);
}

void main_window::update_status()
{
	workspace *ws = current_ws();
	if (!ws)
	{
		m_st_mode->clear();
		m_st_clock->clear();
		m_st_vehicles->clear();
		m_st_speed->clear();
		return;
	}
	const simulation &sim = ws->sim();
	m_st_mode->setText(ws->editor_mode() ? tx("Editor mode (the flow is stopped)") : (ws->running() ? tx("Run") : tx("Pause")));
	m_st_clock->setText(tx("Clock %1").arg(QTime(0, 0).addSecs(static_cast<int>(sim.clock())).toString(QStringLiteral("HH:mm:ss"))));
	m_st_vehicles->setText(tx("Vehicles %1  Pedestrians %2").arg(sim.vehicles().size()).arg(sim.pedestrians().size()));
	m_st_speed->setText(tx("Speed x%1 (real x%2)").arg(ws->speed(), 0, 'g', 3).arg(ws->flowing() ? sim.actual_speed() : 0.0, 0, 'f', 1));
	const logger &log = logger::instance();
	m_st_log->setText(tx("Log: %1").arg(log.enabled() ? logger::level_name(log.min_level()) : tx("off")));
}

void main_window::set_tool(int tool)
{
	if (world_view *v = current_view())
	{
		v->set_tool(static_cast<world_view::tool_t>(tool));
		v->setFocus();
	}
	if (tool >= 0 && tool < world_view::tool_count)
	{
		m_tool_actions[tool]->setChecked(true);
	}
	if (tool == world_view::tool_sign)
	{
		m_signs_dock->show();
		m_signs_dock->raise();
	}
}

void main_window::set_speed(double factor)
{
	if (workspace *ws = current_ws())
	{
		ws->set_speed(factor);
		update_actions();
		update_status();
	}
}

void main_window::tick()
{
	const qint64 now = m_wall.nsecsElapsed();
	const double dt = m_last_ns > 0 ? (now - m_last_ns) / 1.0e9 : 0.0;
	m_last_ns = now;
	for (int i = 0; i < m_tabs->count(); ++i)
	{
		view_at(i)->ws()->tick(dt);
	}
	if (world_view *v = current_view())
	{
		v->update();
	}
	if (m_status_timer.elapsed() >= 250)
	{
		m_status_timer.restart();
		update_status();
		m_props->refresh();
	}
	if (m_chart_timer.elapsed() >= 1000)
	{
		m_chart_timer.restart();
		if (m_charts_dock->isVisible())
		{
			m_charts->refresh();
		}
		if (m_congestion_dock->isVisible())
		{
			m_congestion->refresh();
		}
	}
	if (!m_shot_path.isEmpty())
	{
		take_screenshot();
	}
}

// ---------------------------------------------------------------------------
// dialogs and exports
// ---------------------------------------------------------------------------
void main_window::edit_light_program(int light_id)
{
	workspace *ws = current_ws();
	if (!ws || !ws->model().light(light_id))
	{
		return;
	}
	traffic_light_dialog dlg(ws, light_id, this);
	dlg.exec();
	m_props->rebuild();
	update_tab_titles();
}

void main_window::show_settings()
{
	settings_dialog dlg(this);
	if (dlg.exec() == QDialog::Accepted)
	{
		const int limit = config_common::instance().int_value(QStringLiteral("render/fps_limit"));
		m_timer.setInterval(limit > 0 ? std::max(1, 1000 / limit) : 1);
		a_map->setChecked(config_common::instance().bool_value(QStringLiteral("render/show_map")));
		set_language(config_common::instance().value(QStringLiteral("general/language")));
		m_api->apply_settings();
		if (!m_api->last_error().isEmpty())
		{
			QMessageBox::warning(this, QStringLiteral("API"), tx("The API server cannot start: %1").arg(i18n::tx_message(m_api->last_error())));
		}
		statusBar()->showMessage(tx("The settings are saved. Some settings apply at the next start."), 8000);
		update_status();
	}
}

void main_window::export_json()
{
	workspace *ws = current_ws();
	if (!ws)
	{
		return;
	}
	config_common &cfg = config_common::instance();
	const QString path = QFileDialog::getSaveFileName(this, tx("Export the report"),
		cfg.value(QStringLiteral("general/last_dir")) + QLatin1Char('/') + ws->display_name() + QStringLiteral(".json"),
		tx("JSON file (*.json)"));
	if (path.isEmpty())
	{
		return;
	}
	QSaveFile f(path);
	if (f.open(QIODevice::WriteOnly))
	{
		f.write(json_export::workspace_report(*ws, true));
		if (f.commit())
		{
			cfg.set_value(QStringLiteral("general/last_dir"), QFileInfo(path).absolutePath());
			logger::instance().file_event(QStringLiteral("JSON report exported"), path);
			statusBar()->showMessage(tx("Report saved: %1").arg(path), 6000);
			return;
		}
	}
	QMessageBox::warning(this, tx("Export"), tx("Cannot write the file:\n%1").arg(path));
}

void main_window::import_osm()
{
	osm_import_dialog dlg(this);
	if (dlg.exec() == QDialog::Accepted)
	{
		add_osm_workspace(dlg.model(), dlg.result(), dlg.place_name());
	}
}

void main_window::import_osm_area(double lat, double lon, double size_m)
{
	run_osm_job(QString(), lat, lon, size_m);
}

void main_window::start_api_session(int port)
{
	m_api->start_session(port);
}

void main_window::run_osm_job(const QString &file, double lat, double lon, double size_m)
{
	// Import without the dialog: options of the last import (config.xml).
	osm_job *job = new osm_job(this);
	const QString name = file.isEmpty() ? QStringLiteral("OSM %1, %2").arg(lat, 0, 'f', 4).arg(lon, 0, 'f', 4) : QFileInfo(file).completeBaseName();
	connect(job, &osm_job::finished, this, [this, job, name](bool ok)
	{
		if (ok)
		{
			add_osm_workspace(job->model(), job->result(), name);
		}
		else
		{
			TE_ERROR("OSM", QStringLiteral("Import failed: %1").arg(job->error()));
			statusBar()->showMessage(tx("OpenStreetMap import failed: %1").arg(i18n::tx_message(job->error())), 15000);
			if (m_shot_path.isEmpty())
			{
				QMessageBox::warning(this, tx("Import from OpenStreetMap"), i18n::tx_message(job->error()));
			}
		}
		job->deleteLater();
	});
	statusBar()->showMessage(tx("OpenStreetMap import: %1 ...").arg(name));
	if (file.isEmpty())
	{
		job->start_download(osm_import::area(lat, lon, size_m, size_m), osm_import_dialog::saved_options());
	}
	else
	{
		job->start_file(file, osm_import_dialog::saved_options());
	}
}

void main_window::add_osm_workspace(const std::shared_ptr<world_model> &model, const osm_import_result &result, const QString &name)
{
	if (!model)
	{
		return;
	}
	workspace *ws = new workspace();
	ws->model() = *model;
	if (!name.isEmpty())
	{
		ws->model().name = name;
	}
	ws->model().touch_network();
	ws->model_changed(property_change::change_network);
	ws->reset_simulation();
	add_workspace(ws);
	logger::instance().action(QStringLiteral("OpenStreetMap workspace \"%1\": %2").arg(ws->display_name(), result.summary()));
	statusBar()->showMessage(tx("OpenStreetMap import: %1").arg(result.summary(true)), 20000);
}

void main_window::export_view_png()
{
	world_view *v = current_view();
	if (!v)
	{
		return;
	}
	config_common &cfg = config_common::instance();
	const QString path = QFileDialog::getSaveFileName(this, tx("Export the view"),
		cfg.value(QStringLiteral("general/last_dir")) + QStringLiteral("/view.png"), tx("PNG image (*.png)"));
	if (path.isEmpty())
	{
		return;
	}
	if (v->snapshot().save(path))
	{
		cfg.set_value(QStringLiteral("general/last_dir"), QFileInfo(path).absolutePath());
		logger::instance().file_event(QStringLiteral("View exported"), path);
		statusBar()->showMessage(tx("View saved: %1").arg(path), 6000);
	}
	else
	{
		QMessageBox::warning(this, tx("Export"), tx("Cannot write the file:\n%1").arg(path));
	}
}

void main_window::show_shortcuts()
{
	QString text = QStringLiteral("<table cellpadding='3'>");
	auto row = [&text](const QString &k, const QString &d) { text += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(tx(k), tx(d)); };
	for (int t : tool_order())
	{
		row(m_tool_actions[t]->shortcut().toString(QKeySequence::NativeText), world_view::tool_name(t));
	}
	row(QStringLiteral("F12"), QStringLiteral("Developer details (FPS, timings)"));
	row(QStringLiteral("Ctrl+E"), QStringLiteral("Editor mode on or off (the flow stops in editor mode)"));
	row(QStringLiteral("Space"), QStringLiteral("Run or pause the flow"));
	row(QStringLiteral(". (period)"), QStringLiteral("One simulation step"));
	row(QStringLiteral("+ / -"), QStringLiteral("Faster / slower"));
	row(QStringLiteral("Del"), QStringLiteral("Remove the selected element (editor mode)"));
	row(QStringLiteral("Esc"), QStringLiteral("Cancel the road / select tool / no selection"));
	row(QStringLiteral("Enter, double-click"), QStringLiteral("Finish the road"));
	row(QStringLiteral("Mouse wheel"), QStringLiteral("Zoom in and out at the cursor"));
	row(QStringLiteral("Drag"), QStringLiteral("Move the map (left, middle or right button)"));
	row(QStringLiteral("Double-click a vehicle"), QStringLiteral("The view follows the vehicle"));
	row(QStringLiteral("Ctrl+click on a road"), QStringLiteral("Add a control point to the selected road (editor mode)"));
	text += QStringLiteral("</table>");
	QMessageBox::information(this, tx("Keys and mouse"), text);
}

void main_window::show_about()
{
	QMessageBox::about(this, tx("About Traffic Engine"),
		tx("<h3>Traffic Engine %1</h3>"
			"<p>A 2D traffic simulation engine.</p>"
			"<p>Build: %2<br>Qt: %3<br>Boost: %4<br>Xerces-C: %5</p>"
			"<p>Data folder: %6</p>")
			.arg(QStringLiteral(TE_VERSION), QStringLiteral(TE_BUILD_CONFIG), QString::fromLatin1(qVersion()),
				QStringLiteral(BOOST_LIB_VERSION).replace(QLatin1Char('_'), QLatin1Char('.')),
				QStringLiteral("%1.%2.%3").arg(XERCES_VERSION_MAJOR).arg(XERCES_VERSION_MINOR).arg(XERCES_VERSION_REVISION),
				app_paths::data_dir()));
}

// ---------------------------------------------------------------------------
// window state
// ---------------------------------------------------------------------------
void main_window::show_restored()
{
	const config_common &cfg = config_common::instance();
	const QByteArray geometry = QByteArray::fromBase64(cfg.value(QStringLiteral("window/geometry")).toLatin1());
	const QByteArray state = QByteArray::fromBase64(cfg.value(QStringLiteral("window/state")).toLatin1());
	if (!state.isEmpty())
	{
		restoreState(state);
	}
	if (geometry.isEmpty() || !restoreGeometry(geometry))
	{
		showMaximized();
		return;
	}
	show();
}

void main_window::check_previous_crash()
{
	config_common &cfg = config_common::instance();
	const QStringList reports = crash_handler::reports_after(cfg.value(QStringLiteral("debug/last_crash_seen")));
	if (reports.isEmpty())
	{
		return;
	}
	cfg.set_value(QStringLiteral("debug/last_crash_seen"), crash_handler::stamp_of(reports.first()));
	cfg.save();
	TE_WARN("CRASH", QStringLiteral("The last session stopped unexpectedly. Report: %1").arg(reports.first()));
	if (!m_shot_path.isEmpty())
	{
		return; // test mode: no dialog
	}
	const auto answer = QMessageBox::warning(this, tx("Unexpected stop"),
		tx("Traffic Engine stopped unexpectedly in an earlier session.\n\n"
			"A crash report and a dump file are in this folder:\n%1\n\n"
			"Do you want to open the folder?").arg(app_paths::crash_dir()),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer == QMessageBox::Yes)
	{
		QDesktopServices::openUrl(QUrl::fromLocalFile(app_paths::crash_dir()));
	}
}

void main_window::closeEvent(QCloseEvent *event)
{
	for (int i = m_tabs->count() - 1; i >= 0; --i)
	{
		if (view_at(i)->ws()->dirty() && m_shot_path.isEmpty())
		{
			if (!close_tab(i))
			{
				event->ignore();
				return;
			}
		}
	}
	if (!m_test_mode)
	{
		config_common &cfg = config_common::instance();
		cfg.set_value(QStringLiteral("window/geometry"), QString::fromLatin1(saveGeometry().toBase64()));
		cfg.set_value(QStringLiteral("window/state"), QString::fromLatin1(saveState().toBase64()));
		cfg.save();
	}
	m_timer.stop();
	event->accept();
}

// ---------------------------------------------------------------------------
// test mode
// ---------------------------------------------------------------------------
void main_window::schedule_screenshot(const QString &path, double delay, double speed, bool dev, const QString &select, double zoom, double density)
{
	m_shot_path = path;
	m_shot_delay = delay;
	m_shot_speed = speed;
	m_shot_density = density;
	m_shot_select = select;
	m_shot_selected = false;
	m_shot_timer.start();
	m_test_mode = true;
	if (workspace *ws = current_ws())
	{
		ws->set_speed(speed);
		if (density >= 0.0)
		{
			ws->sim().set_demand_factors(density / 100.0, density / 100.0);
		}
	}
	a_dev->setChecked(dev);
	if (select == QLatin1String("editor"))
	{
		QTimer::singleShot(800, this, [this, path]() { run_editor_script(path); });
	}
	else if (select == QLatin1String("levels") || select == QLatin1String("levels_view"))
	{
		// Before the zoom (300 ms) and the center (500 ms) of the test
		QTimer::singleShot(200, this, [this]() { run_levels_script(); });
	}
	if (zoom > 0.0)
	{
		QTimer::singleShot(300, this, [this, zoom]()
		{
			if (world_view *v = current_view())
			{
				v->zoom_by(zoom / v->cam().ppm, QPointF(v->width() / 2.0, v->height() / 2.0));
			}
		});
	}
	TE_INFO("TEST", QStringLiteral("Screenshot test: %1 after %2 s at x%3, density %4")
		.arg(path).arg(delay).arg(speed).arg(density >= 0.0 ? QStringLiteral("%1%").arg(density) : QStringLiteral("from config.xml")));
}

void main_window::set_shot_view(const QSize &size, int node, const QPointF &center, bool center_valid)
{
	if (size.isValid())
	{
		showNormal();
		resize(size);
	}
	if (node < 0 && !center_valid)
	{
		return;
	}
	// After the zoom of the test (300 ms) and after the new size of the window
	QTimer::singleShot(500, this, [this, node, center, center_valid]()
	{
		world_view *v = current_view();
		if (!v)
		{
			return;
		}
		const node_data *n = node >= 0 ? v->ws()->model().node(node) : nullptr;
		if (n)
		{
			v->center_on(n->pos);
		}
		else if (center_valid)
		{
			v->center_on(vec2(center.x(), center.y()));
		}
		else
		{
			TE_WARN("TEST", QStringLiteral("Screenshot test: node %1 is not found").arg(node));
		}
	});
}

void main_window::set_shot_frames(int count, double interval)
{
	m_shot_frames = std::max(0, count);
	m_shot_frame = 0;
	m_shot_interval = std::max(0.02, interval);
	m_shot_next = -1.0;
}

void main_window::run_editor_script(const QString &shot_path)
{
	// Test of the editor with synthetic mouse clicks (like a user).
	new_workspace(false);
	world_view *v = current_view();
	if (!v)
	{
		return;
	}
	v->center_on(vec2(0.0, 0.0));
	v->zoom_by(2.0 / v->cam().ppm, QPointF(v->width() / 2.0, v->height() / 2.0));
	auto click = [v](double x, double y)
	{
		const QPointF sp = v->cam().to_screen(vec2(x, y));
		QMouseEvent press(QEvent::MouseButtonPress, sp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		QMouseEvent release(QEvent::MouseButtonRelease, sp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(v, &press);
		QApplication::sendEvent(v, &release);
	};
	auto key = [v](int k)
	{
		QKeyEvent ev(QEvent::KeyPress, k, Qt::NoModifier);
		QApplication::sendEvent(v, &ev);
	};
	workspace *ws = v->ws();
	// Road 1: west -> east (Enter finishes)
	set_tool(world_view::tool_road);
	click(-180.0, 0.0);
	click(180.0, 0.0);
	key(Qt::Key_Return);
	// Road 2: from the south to road 1 (the click on road 1 splits it and finishes)
	click(0.0, -170.0);
	click(10.0, -80.0);
	click(0.0, 0.0);
	// Road 3: from the new node to the north
	click(0.0, 0.0);
	click(0.0, 160.0);
	key(Qt::Key_Return);
	// Traffic light at the crossroad, signs and objects
	set_tool(world_view::tool_light);
	click(0.0, 0.0);
	v->set_sign_key(QStringLiteral("speed_limit"));
	set_tool(world_view::tool_sign);
	click(-120.0, -4.0);
	set_tool(world_view::tool_cone);
	click(120.0, -1.7);
	set_tool(world_view::tool_bus_stop);
	click(-5.5, 90.0);
	set_tool(world_view::tool_select);
	const world_model &m = ws->model();
	TE_INFO("TEST", QStringLiteral("Editor script: %1 nodes, %2 roads, %3 lights, %4 signs, %5 objects")
		.arg(m.nodes.size()).arg(m.roads.size()).arg(m.lights.size()).arg(m.signs.size()).arg(m.objects.size()));
	// Save and open again (second tab)
	QString te = shot_path;
	te.replace(QStringLiteral(".png"), QStringLiteral(".te"));
	QString error;
	if (ws->save(te, &error))
	{
		open_files({ te });
		if (workspace *ws2 = current_ws())
		{
			const world_model &m2 = ws2->model();
			TE_INFO("TEST", QStringLiteral("Reopened %1: %2 nodes, %3 roads, %4 lights, %5 signs, %6 objects")
				.arg(te).arg(m2.nodes.size()).arg(m2.roads.size()).arg(m2.lights.size()).arg(m2.signs.size()).arg(m2.objects.size()));
			ws2->set_editor_mode(false);
			ws2->set_running(true);
			ws2->set_speed(8.0);
			if (current_view())
			{
				current_view()->zoom_to_fit();
			}
		}
	}
	else
	{
		TE_ERROR("TEST", QStringLiteral("Editor script: save failed: %1").arg(error));
	}
}

void main_window::run_levels_script()
{
	// Test of the bridge and tunnel tools with synthetic mouse clicks: a
	// ground road from the west to the east, a bridge over it and a tunnel
	// under it. Ground roads lead to the ends of the bridge and the tunnel.
	new_workspace(false);
	world_view *v = current_view();
	if (!v)
	{
		return;
	}
	v->center_on(vec2(0.0, 0.0));
	v->zoom_by(2.0 / v->cam().ppm, QPointF(v->width() / 2.0, v->height() / 2.0));
	auto click = [v](double x, double y)
	{
		const QPointF sp = v->cam().to_screen(vec2(x, y));
		QMouseEvent press(QEvent::MouseButtonPress, sp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		QMouseEvent release(QEvent::MouseButtonRelease, sp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(v, &press);
		QApplication::sendEvent(v, &release);
	};
	auto key = [v](int k)
	{
		QKeyEvent ev(QEvent::KeyPress, k, Qt::NoModifier);
		QApplication::sendEvent(v, &ev);
	};
	auto road = [&](int tool, double x0, double y0, double x1, double y1)
	{
		set_tool(tool);
		click(x0, y0);
		click(x1, y1);
		key(Qt::Key_Return);
	};
	workspace *ws = v->ws();
	road(world_view::tool_road, -260.0, 0.0, 260.0, 0.0);
	// Bridge at x = -120: ground road, bridge over the road at y = 0, ground road
	road(world_view::tool_road, -120.0, -260.0, -120.0, -160.0);
	road(world_view::tool_bridge, -120.0, -160.0, -120.0, 160.0);
	road(world_view::tool_road, -120.0, 160.0, -120.0, 260.0);
	// Tunnel at x = 120: the same with a tunnel under the road
	road(world_view::tool_road, 120.0, -260.0, 120.0, -160.0);
	road(world_view::tool_tunnel, 120.0, -160.0, 120.0, 160.0);
	road(world_view::tool_road, 120.0, 160.0, 120.0, 260.0);
	set_tool(world_view::tool_select);
	const world_model &m = ws->model();
	int bridge = -1;
	QStringList levels;
	for (const auto &kv : m.roads)
	{
		levels << QStringLiteral("%1:%2").arg(kv.first).arg(kv.second.level);
		if (kv.second.level > 0 && bridge < 0)
		{
			bridge = kv.first;
		}
	}
	TE_INFO("TEST", QStringLiteral("Levels script: %1 nodes, %2 roads (road:level %3)").arg(m.nodes.size()).arg(m.roads.size()).arg(levels.join(QLatin1Char(' '))));
	ws->set_editor_mode(false);
	ws->set_running(true);
	if (bridge >= 0 && m_shot_select == QLatin1String("levels"))
	{
		ws->select(element_ref(element_kind::road, bridge)); // the property panel shows the level
	}
}

void main_window::take_screenshot()
{
	const double t = m_shot_timer.elapsed() / 1000.0;
	workspace *ws = current_ws();
	// The selection comes some seconds before the screenshot (a waiting vehicle: just before it)
	const double select_at = m_shot_select == QLatin1String("vehicle") ? m_shot_delay - 0.5 : m_shot_delay * 0.8;
	if (!m_shot_selected && t >= select_at && ws)
	{
		m_shot_selected = true;
		if (m_shot_select == QLatin1String("light") && !ws->model().lights.empty())
		{
			ws->select(element_ref(element_kind::light, ws->model().lights.begin()->first));
		}
		else if (m_shot_select == QLatin1String("vehicle") && !ws->sim().vehicles().empty())
		{
			// A vehicle that waits for another element (the state has a link), else the first one
			int id = ws->sim().vehicles().front().id;
			double best = -1.0;
			for (const sim_vehicle &v : ws->sim().vehicles())
			{
				if (v.cause != wait_cause::none && v.cause_id >= 0 && v.standstill > best)
				{
					best = v.standstill;
					id = v.id;
				}
			}
			ws->select(element_ref(element_kind::vehicle, id));
		}
		else if (m_shot_select == QLatin1String("roads"))
		{
			// Three roads with Ctrl+click (like a user): more selected roads.
			// A vehicle or a sign can be at the point: then the next point of the road.
			world_view *v = current_view();
			int count = 0;
			for (const auto &kv : ws->model().roads)
			{
				const net_road *nr = ws->network().road(kv.first);
				if (!v || !nr || count >= 3)
				{
					continue;
				}
				for (const double f : { 0.5, 0.35, 0.65, 0.2, 0.8 })
				{
					const QPointF sp = v->cam().to_screen(nr->center.point_at(nr->center.length() * f));
					QMouseEvent press(QEvent::MouseButtonPress, sp, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
					QMouseEvent release(QEvent::MouseButtonRelease, sp, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
					QApplication::sendEvent(v, &press);
					QApplication::sendEvent(v, &release);
					const std::vector<int> sel = ws->selected_roads();
					if (std::find(sel.begin(), sel.end(), kv.first) != sel.end())
					{
						++count;
						break;
					}
				}
			}
			TE_INFO("TEST", QStringLiteral("Roads selected with Ctrl+click: %1").arg(ws->selected_roads().size()));
			// Check of the data of the property panel: the fields with different
			// values, then one change for all selected roads
			const std::vector<int> sel = ws->selected_roads();
			if (!sel.empty())
			{
				// One road with another speed limit: the field must have the mark
				property_change one = property_change::change_none;
				world_properties::set(ws->model(), element_ref(element_kind::road, sel.front()), QStringLiteral("speed_limit"), 30.0, &one);
			}
			QStringList mixed;
			for (const property_def &d : world_properties::list_roads(ws->model(), sel))
			{
				if (d.mixed)
				{
					mixed << d.key;
				}
			}
			property_change change = property_change::change_none;
			world_properties::set_roads(ws->model(), sel, QStringLiteral("speed_limit"), 70.0, &change);
			ws->model_changed(change);
			QStringList limits;
			for (int id : sel)
			{
				limits << QStringLiteral("%1:%2").arg(id).arg(ws->model().road(id)->speed_limit);
			}
			TE_INFO("TEST", QStringLiteral("Fields with different values: %1. Speed limits after one change: %2")
				.arg(mixed.isEmpty() ? QStringLiteral("none") : mixed.join(QStringLiteral(", ")), limits.join(QLatin1Char(' '))));
			if (sel.size() >= 2)
			{
				// A road out of the selection and in again
				ws->toggle_road(sel[1]);
				const size_t out_count = ws->selected_roads().size();
				ws->toggle_road(sel[1]);
				TE_INFO("TEST", QStringLiteral("Toggle: %1 roads after a road goes out, %2 after it comes in again")
					.arg(out_count).arg(ws->selected_roads().size()));
			}
		}
		else if (m_shot_select == QLatin1String("charts"))
		{
			m_charts_dock->show();
		}
		else if (m_shot_select == QLatin1String("congestion"))
		{
			a_congestion->setChecked(true);
			if (m_congestion->isVisible())
			{
				m_congestion->refresh();
			}
		}
		else if (m_shot_select == QLatin1String("density"))
		{
			m_density_menu->popup(m_density_button->mapToGlobal(QPoint(0, m_density_button->height())));
		}
		else if (m_shot_select == QLatin1String("lang"))
		{
			// Live change to the other language (config.xml does not change)
			if (!ws->model().lights.empty())
			{
				ws->select(element_ref(element_kind::light, ws->model().lights.begin()->first));
			}
			m_charts_dock->show();
			i18n::set_language(i18n::language() == QLatin1String("tr") ? QStringLiteral("en") : QStringLiteral("tr"));
			apply_language();
			TE_INFO("TEST", QStringLiteral("Language changed to %1").arg(i18n::language()));
		}
		else if (m_shot_select == QLatin1String("program") && !ws->model().lights.empty())
		{
			const int id = ws->model().lights.begin()->first;
			ws->select(element_ref(element_kind::light, id));
			traffic_light_dialog *dlg = new traffic_light_dialog(ws, id, this);
			dlg->setObjectName(QStringLiteral("shot_dialog"));
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			dlg->show();
		}
		else if (m_shot_select == QLatin1String("osm") || m_shot_select == QLatin1String("osm_search") || m_shot_select == QLatin1String("settings"))
		{
			const bool osm = m_shot_select.startsWith(QLatin1String("osm"));
			QDialog *dlg = osm ? static_cast<QDialog *>(new osm_import_dialog(this)) : static_cast<QDialog *>(new settings_dialog(this));
			dlg->setObjectName(QStringLiteral("shot_dialog"));
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			dlg->show();
			if (m_shot_select == QLatin1String("osm"))
			{
				// Shift + drag on the map: a new area (like a user)
				QTimer::singleShot(1500, dlg, [dlg]()
				{
					osm_area_map *map = dlg->findChild<osm_area_map *>();
					if (!map)
					{
						return;
					}
					const QPointF a(map->width() * 0.25, map->height() * 0.30);
					const QPointF b(map->width() * 0.60, map->height() * 0.75);
					QMouseEvent press(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
					QMouseEvent move(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
					QMouseEvent release(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
					QApplication::sendEvent(map, &press);
					QApplication::sendEvent(map, &move);
					QApplication::sendEvent(map, &release);
					TE_INFO("TEST", QStringLiteral("Map area drawn: %1 x %2 m").arg(qRound(map->area_width_m())).arg(qRound(map->area_height_m())));
				});
			}
			else if (m_shot_select == QLatin1String("osm_search"))
			{
				if (QLineEdit *search = dlg->findChild<QLineEdit *>(QStringLiteral("osm_search")))
				{
					search->setText(QStringLiteral("Kadıköy, İstanbul"));
				}
				if (QPushButton *find = dlg->findChild<QPushButton *>(QStringLiteral("osm_find")))
				{
					QTimer::singleShot(500, find, &QPushButton::click);
				}
			}
		}
	}
	if (t < m_shot_delay)
	{
		return;
	}
	// Frames for an animation: one each m_shot_interval seconds of simulated time
	if (m_shot_frame < m_shot_frames && ws)
	{
		world_view *v = current_view();
		const double now = ws->sim().time();
		if (m_shot_next < 0.0)
		{
			m_shot_next = now;
		}
		if (v && now >= m_shot_next)
		{
			QString frame_path = m_shot_path;
			frame_path.replace(QStringLiteral(".png"), QStringLiteral("_f%1.png").arg(m_shot_frame, 3, 10, QLatin1Char('0')));
			v->snapshot().save(frame_path, "PNG", 100); // no compression: fast, the run does not stop
			++m_shot_frame;
			m_shot_next += m_shot_interval;
			if (m_shot_frame == m_shot_frames)
			{
				TE_INFO("TEST", QStringLiteral("Frames saved: %1 (each %2 s of simulated time)").arg(m_shot_frames).arg(m_shot_interval));
			}
		}
		return;
	}
	const QString path = m_shot_path;
	m_shot_path.clear();
	grab().save(path);
	if (m_density_menu->isVisible())
	{
		QString menu_path = path;
		menu_path.replace(QStringLiteral(".png"), QStringLiteral("_density.png"));
		m_density_menu->grab().save(menu_path);
		m_density_menu->close();
	}
	if (QDialog *dlg = findChild<QDialog *>(QStringLiteral("shot_dialog")))
	{
		QString dlg_path = path;
		dlg_path.replace(QStringLiteral(".png"), QStringLiteral("_dialog.png"));
		dlg->grab().save(dlg_path);
		dlg->close();
	}
	if (world_view *v = current_view())
	{
		QString view_path = path;
		view_path.replace(QStringLiteral(".png"), QStringLiteral("_view.png"));
		v->snapshot().save(view_path);
	}
	TE_INFO("TEST", QStringLiteral("Screenshot saved: %1").arg(path));
	if (ws)
	{
		const simulation &sim = ws->sim();
		TE_INFO("TEST", QStringLiteral("Result: clock %1, vehicles %2, completed trips %3, mean speed %4 km/h, junction wait %5 s, real speed x%6, removed (gridlock) %7")
			.arg(QTime(0, 0).addSecs(static_cast<int>(sim.clock())).toString(QStringLiteral("HH:mm:ss")))
			.arg(sim.vehicles().size()).arg(sim.finished_vehicles())
			.arg(sim.stats().last(stats_collector::m_mean_speed), 0, 'f', 1)
			.arg(sim.stats().last(stats_collector::m_junction_wait), 0, 'f', 1)
			.arg(sim.actual_speed(), 0, 'f', 1).arg(sim.gridlock_removed()));
	}
	if (ws)
	{
		ws->set_dirty(false);
	}
	QTimer::singleShot(0, qApp, &QApplication::quit);
}
