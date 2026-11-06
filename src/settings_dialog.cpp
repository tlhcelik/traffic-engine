#include "settings_dialog.h"
#include "app_paths.h"
#include "config_common.h"
#include "i18n.h"
#include "logger.h"
#include "tile_cache.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
	QFormLayout *form_of(QWidget *page)
	{
		return qobject_cast<QFormLayout *>(page->layout());
	}

	QWidget *new_page(QTabWidget *tabs, const QString &title, const QString &note = QString())
	{
		QWidget *page = new QWidget(tabs);
		QFormLayout *f = new QFormLayout(page);
		f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		if (!note.isEmpty())
		{
			QLabel *l = new QLabel(note, page);
			i18n::set_dynamic(l); // the caller translates the note (tx)
			l->setWordWrap(true);
			l->setStyleSheet(QStringLiteral("color: #6B7584;"));
			f->addRow(l);
		}
		tabs->addTab(page, title);
		return page;
	}
}

settings_dialog::settings_dialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle(QStringLiteral("Settings"));
	resize(620, 520);
	QVBoxLayout *root = new QVBoxLayout(this);
	QTabWidget *tabs = new QTabWidget(this);
	root->addWidget(tabs, 1);

	QWidget *general = new_page(tabs, QStringLiteral("General"));
	add_bool(general, QStringLiteral("general/open_demo_on_start"), QStringLiteral("Open the demo workspace at the start"));
	add_bool(general, QStringLiteral("general/show_splash"), QStringLiteral("Show the splash screen at the start"));
	QStringList lang_codes;
	QStringList lang_names;
	for (const auto &l : i18n::languages())
	{
		lang_codes << l.first;
		lang_names << l.second;
	}
	add_choice(general, QStringLiteral("general/language"), QStringLiteral("Language of the user interface"), lang_codes, lang_names);
	i18n::set_dynamic(m_editors.value(QStringLiteral("general/language"))); // native names of the languages

	QWidget *logging = new_page(tabs, QStringLiteral("Logging"),
		tx("The log files are in %1. The logging changes apply immediately.").arg(app_paths::log_dir()));
	add_bool(logging, QStringLiteral("logging/enabled"), QStringLiteral("Write log files"));
	add_choice(logging, QStringLiteral("logging/level"), QStringLiteral("Log level"),
		{ QStringLiteral("trace"), QStringLiteral("debug"), QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error") },
		{ QStringLiteral("Trace (all details)"), QStringLiteral("Debug"), QStringLiteral("Information"), QStringLiteral("Warnings"), QStringLiteral("Errors only") });
	add_bool(logging, QStringLiteral("logging/auto_guard"), QStringLiteral("Lower the level if logging slows the program"));
	add_int(logging, QStringLiteral("logging/guard_lines_per_second"), QStringLiteral("Maximum log lines per second"), 1000, 1000000);
	add_int(logging, QStringLiteral("logging/max_file_mb"), QStringLiteral("Maximum size of a log file"), 1, 2000, QStringLiteral(" MB"));
	add_bool(logging, QStringLiteral("logging/compress_old"), QStringLiteral("Compress old log files with 7-Zip"));
	QPushButton *open_logs = new QPushButton(QStringLiteral("Open the log folder"), logging);
	connect(open_logs, &QPushButton::clicked, this, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(app_paths::log_dir())); });
	form_of(logging)->addRow(open_logs);

	QWidget *debug = new_page(tabs, QStringLiteral("Debug"),
		tx("Debug mode: log level TRACE, each log line goes to the file immediately, full memory dumps and a console window. "
			"A change applies at the next start. The command line argument --debug also starts the debug mode."));
	add_bool(debug, QStringLiteral("debug/enabled"), QStringLiteral("Start in debug mode"));
	add_bool(debug, QStringLiteral("debug/full_memory_dump"), QStringLiteral("Full memory in crash dumps (large files)"));
	QPushButton *open_crash = new QPushButton(QStringLiteral("Open the crash folder"), debug);
	connect(open_crash, &QPushButton::clicked, this, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(app_paths::crash_dir())); });
	form_of(debug)->addRow(open_crash);

	QWidget *render = new_page(tabs, QStringLiteral("Display"),
		tx("Smooth edges (MSAA) and vertical sync apply at the next start."));
	add_choice(render, QStringLiteral("render/msaa_samples"), QStringLiteral("Smooth edges (MSAA)"),
		{ QStringLiteral("0"), QStringLiteral("2"), QStringLiteral("4"), QStringLiteral("8") },
		{ QStringLiteral("Off"), QStringLiteral("2 samples"), QStringLiteral("4 samples"), QStringLiteral("8 samples") });
	add_bool(render, QStringLiteral("render/vsync"), QStringLiteral("Vertical sync (can halve the FPS in remote sessions)"));
	add_int(render, QStringLiteral("render/fps_limit"), QStringLiteral("Frame limit (0 = no limit)"), 0, 500, QStringLiteral(" FPS"));
	add_bool(render, QStringLiteral("render/show_dev_overlay"), QStringLiteral("Show the developer details at the start (F12)"));
	add_bool(render, QStringLiteral("render/show_countdowns"), QStringLiteral("Show the countdowns of the traffic lights"));
	add_bool(render, QStringLiteral("render/show_labels"), QStringLiteral("Show the road names"));
	add_bool(render, QStringLiteral("render/show_grid"), QStringLiteral("Show the grid"));
	add_choice(render, QStringLiteral("render/default_ground"), QStringLiteral("Ground of new workspaces"),
		{ QStringLiteral("grass"), QStringLiteral("soil"), QStringLiteral("sand"), QStringLiteral("snow"), QStringLiteral("concrete"), QStringLiteral("asphalt") },
		{ QStringLiteral("Grass"), QStringLiteral("Soil"), QStringLiteral("Sand"), QStringLiteral("Snow"), QStringLiteral("Concrete"), QStringLiteral("Asphalt") });

	QWidget *sim = new_page(tabs, QStringLiteral("Simulation"),
		tx("The time step and the start speed apply to new workspaces. The traffic density applies "
			"immediately to all open workspaces (100% = the rates of the traffic sources)."));
	add_int(sim, QStringLiteral("simulation/traffic_density"), QStringLiteral("Traffic density: vehicles"), 0, 300, QStringLiteral(" %"));
	add_int(sim, QStringLiteral("simulation/pedestrian_density"), QStringLiteral("Traffic density: pedestrians"), 0, 300, QStringLiteral(" %"));
	add_double(sim, QStringLiteral("simulation/time_step"), QStringLiteral("Time step"), 0.02, 0.5, 0.01, 2, QStringLiteral(" s"));
	add_double(sim, QStringLiteral("simulation/default_speed"), QStringLiteral("Start speed"), 0.1, 64.0, 0.5, 1, QStringLiteral(" x"));
	add_int(sim, QStringLiteral("simulation/max_steps_per_frame"), QStringLiteral("Maximum steps for each frame"), 1, 10000);
	add_double(sim, QStringLiteral("simulation/stats_interval"), QStringLiteral("Statistics interval"), 0.5, 60.0, 0.5, 1, QStringLiteral(" s"));
	add_int(sim, QStringLiteral("simulation/gridlock_timeout"), QStringLiteral("Gridlock: remove a vehicle that waits more than (0 = never)"), 0, 3600, QStringLiteral(" s"));

	QWidget *osm = new_page(tabs, QStringLiteral("Map"),
		tx("The OpenStreetMap import (File > Import from OpenStreetMap) and the map background of the workspaces "
			"with a geographic origin. The tile address must have {z}, {x} and {y}. The servers of openstreetmap.org need "
			"a correct user agent; do not use them for very large areas."));
	add_bool(osm, QStringLiteral("render/show_map"), QStringLiteral("Show the map background (Ctrl+M)"));
	add_int(osm, QStringLiteral("render/map_opacity"), QStringLiteral("Map opacity"), 0, 100, QStringLiteral(" %"));
	add_int(osm, QStringLiteral("render/map_fade"), QStringLiteral("Map pale colors"), 0, 90, QStringLiteral(" %"));
	add_int(osm, QStringLiteral("render/map_max_zoom"), QStringLiteral("Maximum zoom level of the tiles"), 1, 22);
	add_int(osm, QStringLiteral("render/map_cache_days"), QStringLiteral("Keep the tiles for"), 1, 365, QStringLiteral(" days"));
	add_text(osm, QStringLiteral("osm/tile_url"), QStringLiteral("Map tile address"));
	add_text(osm, QStringLiteral("osm/overpass_url"), QStringLiteral("Overpass API address"));
	add_text(osm, QStringLiteral("osm/overpass_mirrors"), QStringLiteral("Other Overpass servers (; between them)"));
	add_text(osm, QStringLiteral("osm/nominatim_url"), QStringLiteral("Place search address (Nominatim)"));
	add_text(osm, QStringLiteral("osm/user_agent"), QStringLiteral("User agent"));
	QPushButton *clear_tiles = new QPushButton(QStringLiteral("Remove the saved map tiles"), osm);
	connect(clear_tiles, &QPushButton::clicked, this, [clear_tiles]()
	{
		tile_cache::instance().clear();
		clear_tiles->setText(tx("The saved map tiles are removed"));
		clear_tiles->setEnabled(false);
	});
	form_of(osm)->addRow(clear_tiles);

	QWidget *api = new_page(tabs, QStringLiteral("API"),
		tx("The external API (HTTP and JSON, see docs/api.md): other programs read the state of the simulation and "
			"send traffic light phases, the traffic density and run / pause commands. Example: GET http://127.0.0.1:8088/api/v1/status. "
			"Use a token if the API listens on all network interfaces."));
	add_bool(api, QStringLiteral("api/enabled"), QStringLiteral("Start the API server"));
	add_int(api, QStringLiteral("api/port"), QStringLiteral("Port"), 1, 65535);
	add_bool(api, QStringLiteral("api/listen_all"), QStringLiteral("Listen on all network interfaces (not only this computer)"));
	add_text(api, QStringLiteral("api/token"), QStringLiteral("Token (empty = no token)"));

	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	root->addWidget(bb);
	connect(bb, &QDialogButtonBox::accepted, this, [this]() { apply(); accept(); });
	connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

	i18n::retranslate(this);
}

void settings_dialog::add_bool(QWidget *page, const QString &key, const QString &label)
{
	QCheckBox *cb = new QCheckBox(page);
	cb->setChecked(config_common::instance().bool_value(key));
	form_of(page)->addRow(label, cb);
	m_editors.insert(key, cb);
}

void settings_dialog::add_int(QWidget *page, const QString &key, const QString &label, int min, int max, const QString &suffix)
{
	QSpinBox *sb = new QSpinBox(page);
	sb->setRange(min, max);
	sb->setValue(config_common::instance().int_value(key));
	sb->setSuffix(suffix);
	form_of(page)->addRow(label, sb);
	m_editors.insert(key, sb);
}

void settings_dialog::add_double(QWidget *page, const QString &key, const QString &label, double min, double max, double step, int decimals, const QString &suffix)
{
	QDoubleSpinBox *sb = new QDoubleSpinBox(page);
	sb->setRange(min, max);
	sb->setSingleStep(step);
	sb->setDecimals(decimals);
	sb->setValue(config_common::instance().double_value(key));
	sb->setSuffix(suffix);
	form_of(page)->addRow(label, sb);
	m_editors.insert(key, sb);
}

void settings_dialog::add_choice(QWidget *page, const QString &key, const QString &label, const QStringList &values, const QStringList &texts)
{
	QComboBox *cb = new QComboBox(page);
	for (int i = 0; i < values.size(); ++i)
	{
		cb->addItem(texts.value(i, values.at(i)), values.at(i));
	}
	const int idx = cb->findData(config_common::instance().value(key));
	cb->setCurrentIndex(idx >= 0 ? idx : 0);
	form_of(page)->addRow(label, cb);
	m_editors.insert(key, cb);
}

void settings_dialog::add_text(QWidget *page, const QString &key, const QString &label)
{
	QLineEdit *le = new QLineEdit(config_common::instance().value(key), page);
	form_of(page)->addRow(label, le);
	m_editors.insert(key, le);
}

void settings_dialog::apply()
{
	config_common &cfg = config_common::instance();
	for (auto it = m_editors.cbegin(); it != m_editors.cend(); ++it)
	{
		QWidget *w = it.value();
		if (QCheckBox *cb = qobject_cast<QCheckBox *>(w))
		{
			cfg.set_bool(it.key(), cb->isChecked());
		}
		else if (QSpinBox *sb = qobject_cast<QSpinBox *>(w))
		{
			cfg.set_int(it.key(), sb->value());
		}
		else if (QDoubleSpinBox *db = qobject_cast<QDoubleSpinBox *>(w))
		{
			cfg.set_double(it.key(), db->value());
		}
		else if (QComboBox *co = qobject_cast<QComboBox *>(w))
		{
			cfg.set_value(it.key(), co->currentData().toString());
		}
		else if (QLineEdit *le = qobject_cast<QLineEdit *>(w))
		{
			cfg.set_value(it.key(), le->text().trimmed());
		}
	}
	cfg.save();
	// Logging: apply now
	logger &log = logger::instance();
	log.set_enabled(cfg.bool_value(QStringLiteral("logging/enabled")));
	log.set_level(logger::level_from_string(cfg.value(QStringLiteral("logging/level"))));
	log.set_auto_guard(cfg.bool_value(QStringLiteral("logging/auto_guard")), cfg.int_value(QStringLiteral("logging/guard_lines_per_second")));
	log.set_max_file_size(qMax(1, cfg.int_value(QStringLiteral("logging/max_file_mb"))) * 1024LL * 1024LL);
}
