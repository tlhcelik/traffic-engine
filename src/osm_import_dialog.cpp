#include "osm_import_dialog.h"
#include "app_paths.h"
#include "config_common.h"
#include "geo.h"
#include "http_client.h"
#include "i18n.h"
#include "logger.h"
#include "osm_area_map.h"
#include "world_model.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <cmath>

// Data that the worker thread and the job share. The worker keeps a
// reference: the job can be deleted before the worker stops.
struct osm_job::state
{
	std::shared_ptr<http_cancel> cancel = std::make_shared<http_cancel>();
	std::atomic<int> phase{ 0 };          // 0 idle, 1 download, 2 file read, 3 conversion
	std::atomic<qint64> received{ 0 };
	std::shared_ptr<world_model> model;
	osm_import_result result;
	QString error;
	bool ok = false;
};

namespace
{
	const qint64 max_osm_bytes = 512LL * 1024 * 1024;

	struct place
	{
		const char *name;
		double lat;
		double lon;
	};

	// Centers of some Turkish cities (UTF-8 names; the compiler uses /utf-8)
	const place places[] =
	{
		{ "Ankara - Kızılay", 39.9208, 32.8541 },
		{ "Ankara - Ulus", 39.9416, 32.8547 },
		{ "İstanbul - Taksim", 41.0369, 28.9850 },
		{ "İstanbul - Kadıköy", 40.9909, 29.0270 },
		{ "İzmir - Konak", 38.4189, 27.1287 },
		{ "Bursa - Heykel", 40.1838, 29.0667 },
		{ "Antalya - Kalekapısı", 36.8865, 30.7056 },
		{ "Eskişehir - Merkez", 39.7767, 30.5206 },
	};

	// Adds the <bounds> element of the area after the <osm ...> tag: an import
	// of the saved file cuts the same area.
	QByteArray with_bounds(const QByteArray &xml, const osm_bounds &b)
	{
		const int start = xml.indexOf("<osm");
		const int end = start >= 0 ? xml.indexOf('>', start) : -1;
		if (end < 0 || xml.indexOf("<bounds") >= 0)
		{
			return xml;
		}
		const QByteArray el = QStringLiteral("\n  <bounds minlat=\"%1\" minlon=\"%2\" maxlat=\"%3\" maxlon=\"%4\"/>")
			.arg(b.min_lat, 0, 'f', 7).arg(b.min_lon, 0, 'f', 7).arg(b.max_lat, 0, 'f', 7).arg(b.max_lon, 0, 'f', 7).toUtf8();
		QByteArray out = xml;
		out.insert(end + 1, el);
		return out;
	}

	// Short text of an HTML or XML error page.
	QString page_text(const QByteArray &body)
	{
		QString t = QString::fromUtf8(body.left(4000));
		t.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
		return t.simplified().left(300);
	}
}

// ---------------------------------------------------------------------------
// osm_job
// ---------------------------------------------------------------------------
osm_job::osm_job(QObject *parent)
	: QObject(parent)
	, m_watcher(new QFutureWatcher<void>(this))
{
	connect(m_watcher, &QFutureWatcher<void>::finished, this, &osm_job::done);
}

osm_job::~osm_job()
{
	if (m_state)
	{
		m_state->cancel->cancel();
	}
}

bool osm_job::busy() const
{
	return m_watcher->isRunning();
}

std::shared_ptr<world_model> osm_job::model() const
{
	return m_state ? m_state->model : nullptr;
}

void osm_job::cancel()
{
	if (m_state)
	{
		m_state->cancel->cancel();
	}
}

QString osm_job::progress_text() const
{
	if (!m_state)
	{
		return QString();
	}
	switch (m_state->phase.load())
	{
		case 1:
		{
			const qint64 kb = m_state->received.load() / 1024;
			return kb > 0 ? tx("Download from the Overpass server: %1 kB").arg(kb)
				: tx("The Overpass server prepares the data (this can take some seconds)...");
		}
		case 2: return tx("Read the file...");
		case 3: return tx("Conversion of the data into roads, junctions and lights...");
		default: return QString();
	}
}

void osm_job::start(const std::function<void()> &work)
{
	m_watcher->setFuture(QtConcurrent::run(work));
}

void osm_job::start_download(const osm_bounds &bounds, const osm_import_options &opt)
{
	if (busy())
	{
		return;
	}
	m_state = std::make_shared<state>();
	const std::shared_ptr<state> st = m_state;
	const config_common &cfg = config_common::instance();
	// The main server, then the other servers (osm/overpass_mirrors). Example:
	// the TLS of overpass-api.de does not work with Windows 10 (no TLS 1.3).
	QStringList servers;
	servers << cfg.value(QStringLiteral("osm/overpass_url")).trimmed();
	for (const QString &m : cfg.value(QStringLiteral("osm/overpass_mirrors")).split(QLatin1Char(';'), QString::SkipEmptyParts))
	{
		if (!servers.contains(m.trimmed()))
		{
			servers << m.trimmed();
		}
	}
	servers.removeAll(QString());
	const QString agent = cfg.value(QStringLiteral("osm/user_agent"));
	const QString query = osm_import::overpass_query(bounds, opt);
	const QString cache = app_paths::data_file(QStringLiteral("osm/last_download.osm"));
	TE_INFO("OSM", QStringLiteral("Overpass query: %1").arg(query.simplified()));
	start([st, servers, agent, query, bounds, opt, cache]()
	{
		st->phase = 1;
		const QByteArray body = QByteArray("data=") + http_client::form_encode(query);
		http_response r;
		QStringList failures;
		for (const QString &url : servers)
		{
			st->received = 0;
			TE_INFO("OSM", QStringLiteral("Download from %1").arg(url));
			r = http_client::request(url, agent, body, QString(), 180000, st->cancel, max_osm_bytes, &st->received);
			if (r.ok() || r.cancelled || (r.status >= 400 && r.status < 500 && r.status != 429))
			{
				break; // success, cancel, or an error of the query: no other server
			}
			const QString why = r.status > 0 ? QStringLiteral("HTTP %1").arg(r.status) : r.error;
			failures << QStringLiteral("%1: %2").arg(url, why);
			TE_WARN("OSM", QStringLiteral("The Overpass server %1 failed (%2). The next server is tried.").arg(url, why));
		}
		if (!r.ok())
		{
			if (r.cancelled)
			{
				st->error = QStringLiteral("The download is cancelled.");
			}
			else if (r.status == 429 || r.status == 503 || r.status == 504)
			{
				st->error = QStringLiteral("The Overpass servers are busy (HTTP %1). Try again after some minutes, or select a smaller area.").arg(r.status);
			}
			else if (r.status > 0)
			{
				st->error = QStringLiteral("The Overpass server sent HTTP %1: %2").arg(r.status).arg(page_text(r.body));
			}
			else
			{
				st->error = QStringLiteral("No connection to an Overpass server:\n%1").arg(failures.join(QLatin1Char('\n')));
			}
			TE_WARN("OSM", st->error);
			return;
		}
		// A copy of the data: an import again needs no new download.
		QDir().mkpath(QFileInfo(cache).absolutePath());
		QSaveFile f(cache);
		if (f.open(QIODevice::WriteOnly))
		{
			f.write(with_bounds(r.body, bounds));
			f.commit();
		}
		st->phase = 3;
		std::shared_ptr<world_model> model = std::make_shared<world_model>();
		QString err;
		if (!osm_import::convert(r.body, bounds, opt, *model, &st->result, &err))
		{
			st->error = err;
			return;
		}
		st->model = model;
		st->ok = true;
	});
}

void osm_job::start_file(const QString &path, const osm_import_options &opt)
{
	if (busy())
	{
		return;
	}
	m_state = std::make_shared<state>();
	const std::shared_ptr<state> st = m_state;
	TE_INFO("OSM", QStringLiteral("Import of the file %1").arg(path));
	start([st, path, opt]()
	{
		st->phase = 2;
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly))
		{
			st->error = QStringLiteral("Cannot open the file: %1").arg(path);
			return;
		}
		if (f.size() > max_osm_bytes)
		{
			st->error = QStringLiteral("The file is too large (more than %1 MB). Use a smaller area.").arg(max_osm_bytes / (1024 * 1024));
			return;
		}
		const QByteArray data = f.readAll();
		st->phase = 3;
		std::shared_ptr<world_model> model = std::make_shared<world_model>();
		QString err;
		if (!osm_import::convert(data, osm_bounds(), opt, *model, &st->result, &err))
		{
			st->error = err;
			return;
		}
		st->model = model;
		st->ok = true;
	});
}

void osm_job::done()
{
	m_result = m_state->result;
	m_error = m_state->error;
	m_state->phase = 0;
	emit finished(m_state->ok);
}

// ---------------------------------------------------------------------------
// osm_import_dialog
// ---------------------------------------------------------------------------
osm_import_dialog::osm_import_dialog(QWidget *parent)
	: QDialog(parent)
	, m_job(new osm_job(this))
	, m_progress_timer(new QTimer(this))
	, m_search_watcher(nullptr)
	, m_syncing(false)
{
	setWindowTitle(QStringLiteral("Import from OpenStreetMap"));
	const config_common &cfg = config_common::instance();
	QVBoxLayout *root = new QVBoxLayout(this);
	QHBoxLayout *content = new QHBoxLayout();
	root->addLayout(content, 1);

	// --- live map with the area (left) ---
	QVBoxLayout *map_column = new QVBoxLayout();
	QHBoxLayout *search_row = new QHBoxLayout();
	m_search = new QLineEdit(this);
	m_search->setPlaceholderText(QStringLiteral("Find a place, for example \"Kızılay, Ankara\""));
	m_search->setClearButtonEnabled(true);
	m_find = new QPushButton(QStringLiteral("Find"), this);
	m_find->setAutoDefault(false);
	m_search->setObjectName(QStringLiteral("osm_search"));
	m_find->setObjectName(QStringLiteral("osm_find"));
	search_row->addWidget(m_search, 1);
	search_row->addWidget(m_find);
	map_column->addLayout(search_row);
	m_map = new osm_area_map(this);
	map_column->addWidget(m_map, 1);
	content->addLayout(map_column, 1);

	// --- fields and options (right) ---
	QVBoxLayout *side = new QVBoxLayout();
	content->addLayout(side, 0);

	// --- source ---
	QGroupBox *src = new QGroupBox(QStringLiteral("Source of the data"), this);
	QGridLayout *sg = new QGridLayout(src);
	m_src_area = new QRadioButton(QStringLiteral("Area from the Overpass API (internet)"), src);
	m_src_file = new QRadioButton(QStringLiteral("OpenStreetMap file (.osm)"), src);
	sg->addWidget(m_src_area, 0, 0, 1, 4);
	m_place = new QComboBox(src);
	m_place->addItem(tx("(the area on the map)"));
	i18n::set_dynamic(m_place); // names of places
	for (const place &p : places)
	{
		m_place->addItem(QString::fromUtf8(p.name));
	}
	m_coords = new QLineEdit(src);
	m_coords->setPlaceholderText(QStringLiteral("Paste \"latitude, longitude\", for example 39.9208, 32.8541"));
	m_lat = new QDoubleSpinBox(src);
	m_lat->setRange(-85.0, 85.0);
	m_lat->setDecimals(6);
	m_lat->setSingleStep(0.001);
	m_lon = new QDoubleSpinBox(src);
	m_lon->setRange(-180.0, 180.0);
	m_lon->setDecimals(6);
	m_lon->setSingleStep(0.001);
	m_width = new QSpinBox(src);
	m_width->setRange(100, 5000);
	m_width->setSingleStep(100);
	m_width->setSuffix(QStringLiteral(" m"));
	m_height = new QSpinBox(src);
	m_height->setRange(100, 5000);
	m_height->setSingleStep(100);
	m_height->setSuffix(QStringLiteral(" m"));
	sg->addWidget(new QLabel(QStringLiteral("Place"), src), 1, 0);
	sg->addWidget(m_place, 1, 1, 1, 3);
	sg->addWidget(new QLabel(QStringLiteral("Coordinates"), src), 2, 0);
	sg->addWidget(m_coords, 2, 1, 1, 3);
	sg->addWidget(new QLabel(QStringLiteral("Latitude"), src), 3, 0);
	sg->addWidget(m_lat, 3, 1);
	sg->addWidget(new QLabel(QStringLiteral("Longitude"), src), 3, 2);
	sg->addWidget(m_lon, 3, 3);
	sg->addWidget(new QLabel(QStringLiteral("Width"), src), 4, 0);
	sg->addWidget(m_width, 4, 1);
	sg->addWidget(new QLabel(QStringLiteral("Height"), src), 4, 2);
	sg->addWidget(m_height, 4, 3);
	sg->addWidget(m_src_file, 5, 0, 1, 4);
	m_file = new QLineEdit(cfg.value(QStringLiteral("osm/last_file")), src);
	m_browse = new QPushButton(QStringLiteral("Browse..."), src);
	sg->addWidget(m_file, 6, 0, 1, 3);
	sg->addWidget(m_browse, 6, 3);
	side->addWidget(src);

	// --- options ---
	QGroupBox *og = new QGroupBox(QStringLiteral("Conversion"), this);
	QGridLayout *ol = new QGridLayout(og);
	m_residential = new QCheckBox(QStringLiteral("Residential streets"), og);
	m_service = new QCheckBox(QStringLiteral("Service roads (no parking aisles)"), og);
	m_lights = new QCheckBox(QStringLiteral("Traffic lights"), og);
	m_priority = new QCheckBox(QStringLiteral("Give way signs on the minor roads"), og);
	m_bus_stops = new QCheckBox(QStringLiteral("Bus stops"), og);
	m_crossings = new QCheckBox(QStringLiteral("Level crossings"), og);
	m_sidewalks = new QCheckBox(QStringLiteral("Sidewalks on urban roads without a sidewalk tag"), og);
	m_priority->setToolTip(QStringLiteral("At a junction without lights, the roads of a lower class (for example residential) give way to the main road."));
	ol->addWidget(m_residential, 0, 0);
	ol->addWidget(m_service, 0, 1);
	ol->addWidget(m_lights, 1, 0);
	ol->addWidget(m_priority, 1, 1);
	ol->addWidget(m_bus_stops, 2, 0);
	ol->addWidget(m_crossings, 2, 1);
	ol->addWidget(m_sidewalks, 3, 0, 1, 2);
	m_join = new QSpinBox(og);
	m_join->setRange(0, 40);
	m_join->setSuffix(QStringLiteral(" m"));
	m_join->setToolTip(QStringLiteral("Junction parts nearer than this distance become one junction (dual carriageways)."));
	m_demand = new QSpinBox(og);
	m_demand->setRange(10, 300);
	m_demand->setSingleStep(10);
	m_demand->setSuffix(QStringLiteral(" %"));
	m_demand->setToolTip(QStringLiteral("Traffic of the sources at the border of the area (100% = rate from the road class and the lanes)."));
	QFormLayout *nf = new QFormLayout();
	nf->addRow(QStringLiteral("Join junction parts nearer than"), m_join);
	nf->addRow(QStringLiteral("Traffic of the sources"), m_demand);
	ol->addLayout(nf, 4, 0, 1, 2);
	side->addWidget(og);
	side->addStretch(1);

	QLabel *note = new QLabel(QStringLiteral("Map data © OpenStreetMap contributors (ODbL). The place search uses Nominatim. "
		"The Overpass API is a free public service: do not download very large areas many times. The last download is "
		"kept for a new import."), this);
	note->setWordWrap(true);
	note->setStyleSheet(QStringLiteral("color: #6B7584;"));
	root->addWidget(note);

	m_status = new QLabel(this);
	i18n::set_dynamic(m_status);
	m_status->setWordWrap(true);
	root->addWidget(m_status);
	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 0);
	m_progress->setTextVisible(false);
	m_progress->setMaximumHeight(8);
	m_progress->hide();
	root->addWidget(m_progress);

	QDialogButtonBox *bb = new QDialogButtonBox(this);
	m_import = bb->addButton(QStringLiteral("Import"), QDialogButtonBox::AcceptRole);
	bb->addButton(QDialogButtonBox::Cancel);
	root->addWidget(bb);

	// --- values of the last import ---
	m_src_area->setChecked(true);
	m_place->setCurrentIndex(qBound(0, cfg.int_value(QStringLiteral("osm/last_place")), m_place->count() - 1));
	m_lat->setValue(cfg.double_value(QStringLiteral("osm/last_lat")));
	m_lon->setValue(cfg.double_value(QStringLiteral("osm/last_lon")));
	m_width->setValue(cfg.int_value(QStringLiteral("osm/last_width")));
	m_height->setValue(cfg.int_value(QStringLiteral("osm/last_height")));
	m_residential->setChecked(cfg.bool_value(QStringLiteral("osm/residential")));
	m_service->setChecked(cfg.bool_value(QStringLiteral("osm/service_roads")));
	m_lights->setChecked(cfg.bool_value(QStringLiteral("osm/lights")));
	m_priority->setChecked(cfg.bool_value(QStringLiteral("osm/priority_signs")));
	m_bus_stops->setChecked(cfg.bool_value(QStringLiteral("osm/bus_stops")));
	m_crossings->setChecked(cfg.bool_value(QStringLiteral("osm/level_crossings")));
	m_sidewalks->setChecked(cfg.bool_value(QStringLiteral("osm/sidewalks")));
	m_join->setValue(cfg.int_value(QStringLiteral("osm/join_distance")));
	m_demand->setValue(cfg.int_value(QStringLiteral("osm/demand")));

	connect(m_place, QOverload<int>::of(&QComboBox::activated), this, [this](int i)
	{
		if (i >= 1 && i - 1 < static_cast<int>(sizeof(places) / sizeof(places[0])))
		{
			m_syncing = true;
			m_lat->setValue(places[i - 1].lat);
			m_lon->setValue(places[i - 1].lon);
			m_syncing = false;
			fields_to_map(true);
		}
	});
	connect(m_coords, &QLineEdit::editingFinished, this, [this]()
	{
		static const QRegularExpression re(QStringLiteral("(-?\\d+(?:\\.\\d+)?)\\s*[,; ]\\s*(-?\\d+(?:\\.\\d+)?)"));
		const QRegularExpressionMatch m = re.match(m_coords->text());
		if (m.hasMatch())
		{
			m_syncing = true;
			m_lat->setValue(m.captured(1).toDouble());
			m_lon->setValue(m.captured(2).toDouble());
			m_place->setCurrentIndex(0);
			m_syncing = false;
			fields_to_map(true);
		}
	});
	// A typed value moves the area on the map (live).
	auto edited = [this]()
	{
		if (m_syncing)
		{
			return;
		}
		m_place->setCurrentIndex(0);
		fields_to_map(false);
	};
	connect(m_lat, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, edited);
	connect(m_lon, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, edited);
	connect(m_width, QOverload<int>::of(&QSpinBox::valueChanged), this, edited);
	connect(m_height, QOverload<int>::of(&QSpinBox::valueChanged), this, edited);
	// The area on the map changes the fields (live, also during the drag).
	connect(m_map, &osm_area_map::area_changed, this, &osm_import_dialog::map_to_fields);
	connect(m_find, &QPushButton::clicked, this, &osm_import_dialog::find_place);
	connect(m_search, &QLineEdit::returnPressed, this, &osm_import_dialog::find_place);
	connect(m_browse, &QPushButton::clicked, this, [this]()
	{
		const QString f = QFileDialog::getOpenFileName(this, tx("OpenStreetMap file"),
			m_file->text().isEmpty() ? config_common::instance().value(QStringLiteral("general/last_dir")) : m_file->text(),
			tx("OpenStreetMap XML (*.osm *.xml)"));
		if (!f.isEmpty())
		{
			m_file->setText(QDir::toNativeSeparators(f));
			m_src_file->setChecked(true);
		}
	});
	connect(m_src_area, &QRadioButton::toggled, this, [this]() { update_state(); });
	connect(m_import, &QPushButton::clicked, this, &osm_import_dialog::start_import);
	connect(bb, &QDialogButtonBox::rejected, this, &osm_import_dialog::reject);
	connect(m_job, &osm_job::finished, this, &osm_import_dialog::job_done);
	connect(m_progress_timer, &QTimer::timeout, this, [this]() { m_status->setText(m_job->progress_text()); });
	fields_to_map(true);
	update_state();
	resize(1180, 760);
	i18n::retranslate(this);
}

osm_import_dialog::~osm_import_dialog()
{
	if (m_search_cancel)
	{
		m_search_cancel->cancel(); // a running place search stops
	}
}

void osm_import_dialog::fields_to_map(bool fit)
{
	if (m_syncing)
	{
		return;
	}
	m_map->set_area(osm_import::area(m_lat->value(), m_lon->value(), m_width->value(), m_height->value()), fit);
}

void osm_import_dialog::map_to_fields(const osm_bounds &b)
{
	// Center: the middle of the Mercator rectangle (the import projection uses this point).
	const double my = (geo_projection::mercator_y(b.min_lat) + geo_projection::mercator_y(b.max_lat)) / 2.0;
	const double lat = (2.0 * std::atan(std::exp(my / geo_projection::earth_radius)) - M_PI / 2.0) * 180.0 / M_PI;
	const double lon = (b.min_lon + b.max_lon) / 2.0;
	m_syncing = true;
	m_lat->setValue(lat);
	m_lon->setValue(lon);
	m_width->setValue(qRound(m_map->area_width_m()));
	m_height->setValue(qRound(m_map->area_height_m()));
	m_place->setCurrentIndex(0);
	if (!m_src_area->isChecked())
	{
		m_src_area->setChecked(true);
	}
	m_syncing = false;
	TE_TRACE("OSM", QStringLiteral("Import area from the map: center %1, %2, %3 x %4 m")
		.arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6).arg(m_width->value()).arg(m_height->value()));
}

void osm_import_dialog::find_place()
{
	const QString q = m_search->text().trimmed();
	if (q.isEmpty() || m_search_watcher)
	{
		return; // one search at a time (usage policy of Nominatim: maximum one request each second)
	}
	const config_common &cfg = config_common::instance();
	const QString url = cfg.value(QStringLiteral("osm/nominatim_url")).trimmed()
		+ QStringLiteral("?format=jsonv2&limit=1&accept-language=tr,en&q=") + QString::fromLatin1(QUrl::toPercentEncoding(q));
	const QString agent = cfg.value(QStringLiteral("osm/user_agent"));
	m_search_cancel = std::make_shared<http_cancel>();
	const std::shared_ptr<http_cancel> cancel = m_search_cancel;
	m_find->setEnabled(false);
	m_status->setStyleSheet(QString());
	m_status->setText(tx("Search for \"%1\"...").arg(q));
	TE_INFO("OSM", QStringLiteral("Place search: %1").arg(q));
	m_search_watcher = new QFutureWatcher<http_response>(this);
	connect(m_search_watcher, &QFutureWatcherBase::finished, this, &osm_import_dialog::place_found);
	m_search_watcher->setFuture(QtConcurrent::run([url, agent, cancel]()
	{
		return http_client::request(url, agent, QByteArray(), QString(), 20000, cancel, 2 * 1024 * 1024);
	}));
}

void osm_import_dialog::place_found()
{
	const http_response r = m_search_watcher->result();
	m_search_watcher->deleteLater();
	m_search_watcher = nullptr;
	m_find->setEnabled(true);
	if (!r.ok())
	{
		m_status->setStyleSheet(QStringLiteral("color: #C0392B;"));
		m_status->setText(tx("The place search failed: %1").arg(r.status > 0 ? QStringLiteral("HTTP %1").arg(r.status) : i18n::tx_message(r.error)));
		return;
	}
	const QJsonArray found = QJsonDocument::fromJson(r.body).array();
	if (found.isEmpty())
	{
		m_status->setStyleSheet(QStringLiteral("color: #C0392B;"));
		m_status->setText(tx("No place is found for \"%1\".").arg(m_search->text().trimmed()));
		return;
	}
	const QJsonObject o = found.first().toObject();
	const double lat = o.value(QStringLiteral("lat")).toString().toDouble();
	const double lon = o.value(QStringLiteral("lon")).toString().toDouble();
	// The box of the place if it has a good size for an import, else the current size around the place.
	osm_bounds b = osm_import::area(lat, lon, m_width->value(), m_height->value());
	const QJsonArray box = o.value(QStringLiteral("boundingbox")).toArray();
	if (box.size() == 4)
	{
		osm_bounds p;
		p.min_lat = box.at(0).toString().toDouble();
		p.max_lat = box.at(1).toString().toDouble();
		p.min_lon = box.at(2).toString().toDouble();
		p.max_lon = box.at(3).toString().toDouble();
		const geo_projection proj(lat, lon);
		const vec2 lo = proj.to_world(p.min_lat, p.min_lon);
		const vec2 hi = proj.to_world(p.max_lat, p.max_lon);
		const double w = hi.x - lo.x;
		const double h = hi.y - lo.y;
		if (p.valid() && w >= 300.0 && h >= 300.0 && w <= osm_area_map::max_size_m && h <= osm_area_map::max_size_m)
		{
			b = p;
		}
	}
	m_map->set_area(b, true);
	map_to_fields(m_map->area());
	m_status->setStyleSheet(QString());
	m_status->setText(tx("Found: %1").arg(o.value(QStringLiteral("display_name")).toString()));
	TE_INFO("OSM", QStringLiteral("Place found: %1 (%2, %3)").arg(o.value(QStringLiteral("display_name")).toString()).arg(lat, 0, 'f', 6).arg(lon, 0, 'f', 6));
}

std::shared_ptr<world_model> osm_import_dialog::model() const
{
	return m_job->model();
}

osm_import_result osm_import_dialog::result() const
{
	return m_job->result();
}

QString osm_import_dialog::place_name() const
{
	return m_place_name;
}

osm_import_options osm_import_dialog::saved_options()
{
	const config_common &cfg = config_common::instance();
	osm_import_options o;
	o.residential = cfg.bool_value(QStringLiteral("osm/residential"));
	o.service_roads = cfg.bool_value(QStringLiteral("osm/service_roads"));
	o.lights = cfg.bool_value(QStringLiteral("osm/lights"));
	o.priority_signs = cfg.bool_value(QStringLiteral("osm/priority_signs"));
	o.bus_stops = cfg.bool_value(QStringLiteral("osm/bus_stops"));
	o.level_crossings = cfg.bool_value(QStringLiteral("osm/level_crossings"));
	o.sidewalks = cfg.bool_value(QStringLiteral("osm/sidewalks"));
	o.join_distance = cfg.int_value(QStringLiteral("osm/join_distance"));
	o.demand_factor = cfg.int_value(QStringLiteral("osm/demand")) / 100.0;
	return o;
}

osm_import_options osm_import_dialog::options() const
{
	osm_import_options o;
	o.residential = m_residential->isChecked();
	o.service_roads = m_service->isChecked();
	o.lights = m_lights->isChecked();
	o.priority_signs = m_priority->isChecked();
	o.bus_stops = m_bus_stops->isChecked();
	o.level_crossings = m_crossings->isChecked();
	o.sidewalks = m_sidewalks->isChecked();
	o.join_distance = m_join->value();
	o.demand_factor = m_demand->value() / 100.0;
	return o;
}

void osm_import_dialog::save_settings() const
{
	config_common &cfg = config_common::instance();
	cfg.set_int(QStringLiteral("osm/last_place"), m_place->currentIndex());
	cfg.set_double(QStringLiteral("osm/last_lat"), m_lat->value());
	cfg.set_double(QStringLiteral("osm/last_lon"), m_lon->value());
	cfg.set_int(QStringLiteral("osm/last_width"), m_width->value());
	cfg.set_int(QStringLiteral("osm/last_height"), m_height->value());
	cfg.set_value(QStringLiteral("osm/last_file"), m_file->text().trimmed());
	cfg.set_bool(QStringLiteral("osm/residential"), m_residential->isChecked());
	cfg.set_bool(QStringLiteral("osm/service_roads"), m_service->isChecked());
	cfg.set_bool(QStringLiteral("osm/lights"), m_lights->isChecked());
	cfg.set_bool(QStringLiteral("osm/priority_signs"), m_priority->isChecked());
	cfg.set_bool(QStringLiteral("osm/bus_stops"), m_bus_stops->isChecked());
	cfg.set_bool(QStringLiteral("osm/level_crossings"), m_crossings->isChecked());
	cfg.set_bool(QStringLiteral("osm/sidewalks"), m_sidewalks->isChecked());
	cfg.set_int(QStringLiteral("osm/join_distance"), m_join->value());
	cfg.set_int(QStringLiteral("osm/demand"), m_demand->value());
	cfg.save();
}

void osm_import_dialog::update_state()
{
	const bool busy = m_job->busy();
	const bool area = m_src_area->isChecked();
	for (QWidget *w : std::initializer_list<QWidget *>{ m_place, m_coords, m_lat, m_lon, m_width, m_height })
	{
		w->setEnabled(area && !busy);
	}
	m_file->setEnabled(!area && !busy);
	m_browse->setEnabled(!area && !busy);
	for (QWidget *w : std::initializer_list<QWidget *>{ m_src_area, m_src_file, m_residential, m_service, m_lights, m_priority,
		m_bus_stops, m_crossings, m_sidewalks, m_join, m_demand })
	{
		w->setEnabled(!busy);
	}
	m_import->setEnabled(!busy);
	m_map->setEnabled(!busy);
	m_search->setEnabled(!busy);
	m_find->setEnabled(!busy && !m_search_watcher);
	m_progress->setVisible(busy);
}

void osm_import_dialog::start_import()
{
	if (m_job->busy())
	{
		return;
	}
	save_settings();
	m_status->setStyleSheet(QString());
	const osm_import_options opt = options();
	if (m_src_area->isChecked())
	{
		const osm_bounds b = osm_import::area(m_lat->value(), m_lon->value(), m_width->value(), m_height->value());
		m_place_name = m_place->currentIndex() > 0 ? m_place->currentText()
			: QStringLiteral("OSM %1, %2").arg(m_lat->value(), 0, 'f', 4).arg(m_lon->value(), 0, 'f', 4);
		logger::instance().action(QStringLiteral("OpenStreetMap import: %1 (%2 x %3 m)").arg(m_place_name).arg(m_width->value()).arg(m_height->value()));
		m_job->start_download(b, opt);
	}
	else
	{
		const QString path = m_file->text().trimmed();
		if (path.isEmpty() || !QFileInfo::exists(path))
		{
			m_status->setStyleSheet(QStringLiteral("color: #C0392B;"));
			m_status->setText(tx("Select an existing .osm file."));
			return;
		}
		m_place_name = QFileInfo(path).completeBaseName();
		logger::instance().action(QStringLiteral("OpenStreetMap import: file %1").arg(path));
		m_job->start_file(path, opt);
	}
	m_status->setText(m_job->progress_text());
	m_progress_timer->start(200);
	update_state();
}

void osm_import_dialog::job_done(bool ok)
{
	m_progress_timer->stop();
	update_state();
	if (ok)
	{
		accept();
		return;
	}
	m_status->setStyleSheet(QStringLiteral("color: #C0392B;"));
	m_status->setText(i18n::tx_message(m_job->error())); // the log has the English text
}

void osm_import_dialog::reject()
{
	if (m_job->busy())
	{
		m_job->cancel();
	}
	QDialog::reject();
}
