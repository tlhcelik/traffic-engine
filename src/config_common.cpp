#include "config_common.h"
#include "xml_doc.h"
#include "logger.h"

#include <QFileInfo>

namespace
{
	const int max_recent_files = 10;

	QString section_of(const QString &key)
	{
		return key.section(QLatin1Char('/'), 0, 0);
	}

	QString name_of(const QString &key)
	{
		return key.section(QLatin1Char('/'), 1);
	}
}

const QMap<QString, QString> &config_common::defaults()
{
	static const QMap<QString, QString> d =
	{
		{ QStringLiteral("general/last_dir"), QString() },
		{ QStringLiteral("general/open_demo_on_start"), QStringLiteral("true") },
		{ QStringLiteral("general/language"), QStringLiteral("en") },
		{ QStringLiteral("general/show_splash"), QStringLiteral("true") },
		{ QStringLiteral("general/splash_fade_ms"), QStringLiteral("400") },
		{ QStringLiteral("general/splash_hold_ms"), QStringLiteral("1500") },
		{ QStringLiteral("general/config_revision"), QStringLiteral("2") },

		{ QStringLiteral("logging/enabled"), QStringLiteral("true") },
		{ QStringLiteral("logging/level"), QStringLiteral("info") },
		{ QStringLiteral("logging/auto_guard"), QStringLiteral("true") },
		{ QStringLiteral("logging/guard_lines_per_second"), QStringLiteral("20000") },
		{ QStringLiteral("logging/max_file_mb"), QStringLiteral("100") },
		{ QStringLiteral("logging/compress_old"), QStringLiteral("true") },

		{ QStringLiteral("debug/enabled"), QStringLiteral("false") },
		{ QStringLiteral("debug/full_memory_dump"), QStringLiteral("false") },
		{ QStringLiteral("debug/last_crash_seen"), QString() },

		{ QStringLiteral("render/msaa_samples"), QStringLiteral("4") },
		{ QStringLiteral("render/vsync"), QStringLiteral("false") },
		{ QStringLiteral("render/fps_limit"), QStringLiteral("60") },
		{ QStringLiteral("render/show_dev_overlay"), QStringLiteral("false") },
		{ QStringLiteral("render/show_countdowns"), QStringLiteral("true") },
		{ QStringLiteral("render/show_labels"), QStringLiteral("true") },
		{ QStringLiteral("render/show_grid"), QStringLiteral("false") },
		{ QStringLiteral("render/default_ground"), QStringLiteral("grass") },
		{ QStringLiteral("render/show_map"), QStringLiteral("true") },
		{ QStringLiteral("render/map_opacity"), QStringLiteral("100") },
		{ QStringLiteral("render/map_fade"), QStringLiteral("20") },
		{ QStringLiteral("render/map_max_zoom"), QStringLiteral("19") },
		{ QStringLiteral("render/map_cache_days"), QStringLiteral("30") },

		{ QStringLiteral("simulation/time_step"), QStringLiteral("0.1") },
		{ QStringLiteral("simulation/default_speed"), QStringLiteral("1") },
		{ QStringLiteral("simulation/max_steps_per_frame"), QStringLiteral("400") },
		{ QStringLiteral("simulation/start_clock"), QStringLiteral("08:00:00") },
		{ QStringLiteral("simulation/default_spawn_rate"), QStringLiteral("300") },
		{ QStringLiteral("simulation/traffic_density"), QStringLiteral("100") },
		{ QStringLiteral("simulation/pedestrian_density"), QStringLiteral("100") },
		{ QStringLiteral("simulation/gridlock_timeout"), QStringLiteral("180") },
		{ QStringLiteral("simulation/stats_interval"), QStringLiteral("1") },

		{ QStringLiteral("api/enabled"), QStringLiteral("false") },
		{ QStringLiteral("api/port"), QStringLiteral("8088") },
		{ QStringLiteral("api/listen_all"), QStringLiteral("false") },
		{ QStringLiteral("api/token"), QString() },

		{ QStringLiteral("window/geometry"), QString() },
		{ QStringLiteral("window/state"), QString() },

		{ QStringLiteral("osm/overpass_url"), QStringLiteral("https://overpass-api.de/api/interpreter") },
		{ QStringLiteral("osm/overpass_mirrors"), QStringLiteral("https://maps.mail.ru/osm/tools/overpass/api/interpreter;http://overpass-api.de/api/interpreter") },
		{ QStringLiteral("osm/nominatim_url"), QStringLiteral("https://nominatim.openstreetmap.org/search") },
		{ QStringLiteral("osm/tile_url"), QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png") },
		{ QStringLiteral("osm/user_agent"), QStringLiteral("TrafficEngine/" TE_VERSION " (Windows; traffic simulation)") },
		{ QStringLiteral("osm/last_place"), QStringLiteral("1") },
		{ QStringLiteral("osm/last_lat"), QStringLiteral("39.9208") },
		{ QStringLiteral("osm/last_lon"), QStringLiteral("32.8541") },
		{ QStringLiteral("osm/last_width"), QStringLiteral("1000") },
		{ QStringLiteral("osm/last_height"), QStringLiteral("1000") },
		{ QStringLiteral("osm/last_file"), QString() },
		{ QStringLiteral("osm/residential"), QStringLiteral("true") },
		{ QStringLiteral("osm/service_roads"), QStringLiteral("false") },
		{ QStringLiteral("osm/lights"), QStringLiteral("true") },
		{ QStringLiteral("osm/priority_signs"), QStringLiteral("true") },
		{ QStringLiteral("osm/bus_stops"), QStringLiteral("true") },
		{ QStringLiteral("osm/level_crossings"), QStringLiteral("true") },
		{ QStringLiteral("osm/sidewalks"), QStringLiteral("true") },
		{ QStringLiteral("osm/join_distance"), QStringLiteral("18") },
		{ QStringLiteral("osm/demand"), QStringLiteral("100") },
	};
	return d;
}

config_common::config_common()
	: m_values(defaults())
{
}

config_common &config_common::instance()
{
	static config_common inst;
	return inst;
}

QString config_common::config_file_path() const
{
	return m_path;
}

QString config_common::last_error() const
{
	return m_error;
}

bool config_common::load(const QString &file_path)
{
	m_path = file_path;
	m_values = defaults();
	m_recent.clear();

	if (!QFileInfo::exists(file_path))
	{
		return save();
	}

	xml_document doc;
	if (!doc.load(file_path, &m_error))
	{
		// A damaged file must not stop the program: use the default values.
		return false;
	}
	const xml_element root = doc.root();
	for (auto it = m_values.begin(); it != m_values.end(); ++it)
	{
		const xml_element sec = root.first_child(section_of(it.key()));
		if (!sec.is_null() && sec.has_attribute(name_of(it.key())))
		{
			it.value() = sec.attribute(name_of(it.key()));
		}
	}
	// Migration of old files: change only the values that are still the old defaults.
	const xml_element gen = root.first_child(QStringLiteral("general"));
	if (gen.attribute_int(QStringLiteral("config_revision"), 1) < 2)
	{
		auto migrate = [this](const QString &key, const QString &old_default, const QString &new_value)
		{
			if (m_values.value(key) == old_default)
			{
				m_values[key] = new_value;
			}
		};
		migrate(QStringLiteral("render/vsync"), QStringLiteral("true"), QStringLiteral("false"));
		migrate(QStringLiteral("render/fps_limit"), QStringLiteral("0"), QStringLiteral("60"));
		migrate(QStringLiteral("simulation/default_spawn_rate"), QStringLiteral("600"), QStringLiteral("300"));
		m_values[QStringLiteral("general/config_revision")] = QStringLiteral("2");
	}
	const xml_element rf = root.first_child(QStringLiteral("recent_files"));
	for (const xml_element &f : rf.children(QStringLiteral("file")))
	{
		const QString p = f.text().trimmed();
		if (!p.isEmpty() && !m_recent.contains(p) && m_recent.size() < max_recent_files)
		{
			m_recent.append(p);
		}
	}
	return true;
}

bool config_common::save()
{
	if (m_path.isEmpty())
	{
		return false;
	}
	xml_document doc;
	if (!doc.create(QStringLiteral("config")))
	{
		m_error = QStringLiteral("Cannot make the XML document.");
		return false;
	}
	xml_element root = doc.root();
	root.set_attribute(QStringLiteral("version"), 1);
	root.set_attribute(QStringLiteral("program_version"), QStringLiteral(TE_VERSION));

	QMap<QString, xml_element> sections;
	for (auto it = m_values.cbegin(); it != m_values.cend(); ++it)
	{
		const QString sec_name = section_of(it.key());
		if (!sections.contains(sec_name))
		{
			sections.insert(sec_name, root.add_child(sec_name));
		}
		sections[sec_name].set_attribute(name_of(it.key()), it.value());
	}
	xml_element rf = root.add_child(QStringLiteral("recent_files"));
	for (const QString &p : m_recent)
	{
		rf.add_child(QStringLiteral("file")).set_text(p);
	}
	return doc.save(m_path, &m_error);
}

QString config_common::value(const QString &key) const
{
	return m_values.value(key, defaults().value(key));
}

bool config_common::bool_value(const QString &key) const
{
	const QString v = value(key).trimmed().toLower();
	return v == QLatin1String("true") || v == QLatin1String("1") || v == QLatin1String("yes");
}

int config_common::int_value(const QString &key) const
{
	bool ok = false;
	const int v = value(key).trimmed().toInt(&ok);
	return ok ? v : defaults().value(key).toInt();
}

double config_common::double_value(const QString &key) const
{
	bool ok = false;
	const double v = value(key).trimmed().toDouble(&ok);
	return ok ? v : defaults().value(key).toDouble();
}

void config_common::set_value(const QString &key, const QString &value)
{
	const QString old = this->value(key);
	if (old == value && m_values.contains(key))
	{
		return;
	}
	m_values[key] = value;
	// Window geometry is binary data (base64): do not write it to the log.
	if (!key.startsWith(QLatin1String("window/")))
	{
		logger::instance().settings_change(key, old, value);
	}
	emit changed(key);
}

void config_common::set_bool(const QString &key, bool value)
{
	set_value(key, value ? QStringLiteral("true") : QStringLiteral("false"));
}

void config_common::set_int(const QString &key, int value)
{
	set_value(key, QString::number(value));
}

void config_common::set_double(const QString &key, double value)
{
	set_value(key, QString::number(value, 'g', 10));
}

QStringList config_common::keys() const
{
	return m_values.keys();
}

QStringList config_common::recent_files() const
{
	return m_recent;
}

void config_common::add_recent_file(const QString &path)
{
	m_recent.removeAll(path);
	m_recent.prepend(path);
	while (m_recent.size() > max_recent_files)
	{
		m_recent.removeLast();
	}
	emit recent_files_changed();
}

void config_common::remove_recent_file(const QString &path)
{
	if (m_recent.removeAll(path) > 0)
	{
		emit recent_files_changed();
	}
}
