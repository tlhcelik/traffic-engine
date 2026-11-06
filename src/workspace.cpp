#include "workspace.h"
#include "config_common.h"
#include "debug_tools.h"
#include "i18n.h"
#include "logger.h"
#include "te_file.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>

namespace
{
	int g_untitled_counter = 0;

	// Global traffic density (percent in config.xml) -> factors of the simulation.
	void apply_density(simulation &sim)
	{
		const config_common &cfg = config_common::instance();
		sim.set_demand_factors(cfg.int_value(QStringLiteral("simulation/traffic_density")) / 100.0,
			cfg.int_value(QStringLiteral("simulation/pedestrian_density")) / 100.0);
	}
}

workspace::workspace(QObject *parent)
	: QObject(parent)
	, m_dirty(false)
	, m_editor(false)
	, m_running(true)
	, m_speed(1.0)
	, m_untitled(++g_untitled_counter)
{
	const config_common &cfg = config_common::instance();
	m_speed = qBound(0.1, cfg.double_value(QStringLiteral("simulation/default_speed")), 64.0);
	m_sim.set_time_step(cfg.double_value(QStringLiteral("simulation/time_step")));
	m_sim.set_stats_interval(cfg.double_value(QStringLiteral("simulation/stats_interval")));
	m_sim.set_gridlock_timeout(cfg.double_value(QStringLiteral("simulation/gridlock_timeout")));
	m_model.ground = ground_from_name(cfg.value(QStringLiteral("render/default_ground")));
	m_net.build(m_model);
	m_sim.reset(&m_model, &m_net);
	apply_density(m_sim);
	// Debug mode: the checks of the simulation write overlapping and stuck
	// vehicles (with their wait chains) to the debug log.
	m_sim.set_checks(debug_tools::is_debug_mode());
	connect(&config_common::instance(), &config_common::changed, this, [this](const QString &key)
	{
		if (key == QLatin1String("simulation/traffic_density") || key == QLatin1String("simulation/pedestrian_density"))
		{
			apply_density(m_sim);
		}
		else if (key == QLatin1String("simulation/gridlock_timeout"))
		{
			m_sim.set_gridlock_timeout(config_common::instance().double_value(key));
		}
	});
}

workspace::~workspace() = default;

void workspace::set_file_path(const QString &path)
{
	m_path = path;
	emit changed();
}

QString workspace::display_name() const
{
	if (!m_path.isEmpty())
	{
		return QFileInfo(m_path).completeBaseName();
	}
	if (!m_model.name.isEmpty())
	{
		return m_model.name;
	}
	return tx("New workspace %1").arg(m_untitled);
}

void workspace::set_dirty(bool on)
{
	if (m_dirty != on)
	{
		m_dirty = on;
		emit changed();
	}
}

void workspace::set_editor_mode(bool on)
{
	if (m_editor == on)
	{
		return;
	}
	m_editor = on;
	TE_INFO("ACTION", QStringLiteral("%1: %2").arg(display_name(), on ? QStringLiteral("editor mode on (the flow stops)") : QStringLiteral("editor mode off (the flow continues)")));
	if (!on)
	{
		sync_network();
	}
	emit mode_changed();
}

void workspace::set_running(bool on)
{
	if (m_running == on)
	{
		return;
	}
	m_running = on;
	TE_INFO("ACTION", QStringLiteral("%1: %2").arg(display_name(), on ? QStringLiteral("run") : QStringLiteral("pause")));
	emit mode_changed();
}

void workspace::set_speed(double factor)
{
	const double f = qBound(0.1, factor, 64.0);
	if (qFuzzyCompare(f, m_speed))
	{
		return;
	}
	m_speed = f;
	TE_INFO("ACTION", QStringLiteral("%1: speed x%2").arg(display_name()).arg(f));
	emit mode_changed();
}

void workspace::model_changed(property_change change)
{
	if (change == property_change::change_attributes)
	{
		m_net.refresh_attributes(m_model);
	}
	sync_network();
	validate_selection();
	set_dirty(true);
	emit changed();
}

void workspace::sync_network()
{
	if (m_net.built_revision() == m_model.network_revision)
	{
		return;
	}
	QElapsedTimer t;
	t.start();
	m_sim.network_changing();
	m_net.build(m_model);
	m_sim.network_changed();
	TE_DEBUG("EDIT", QStringLiteral("%1: road network made again in %2 ms").arg(display_name()).arg(t.elapsed()));
	emit network_rebuilt();
}

void workspace::tick(double wall_dt)
{
	sync_network();
	if (flowing())
	{
		const int max_steps = qMax(1, config_common::instance().int_value(QStringLiteral("simulation/max_steps_per_frame")));
		m_sim.advance(wall_dt, m_speed, max_steps);
		if (m_selection.kind == element_kind::vehicle || m_selection.kind == element_kind::pedestrian)
		{
			validate_selection();
		}
	}
}

void workspace::reset_simulation()
{
	sync_network();
	m_sim.reset(&m_model, &m_net);
	validate_selection();
	emit mode_changed();
}

void workspace::single_step()
{
	sync_network();
	m_sim.step(m_sim.time_step());
}

void workspace::select(const element_ref &ref)
{
	if (ref == m_selection && m_more_roads.empty())
	{
		return;
	}
	m_selection = ref;
	m_more_roads.clear();
	emit selection_changed(ref);
}

void workspace::toggle_road(int road_id)
{
	if (!m_model.road(road_id))
	{
		return;
	}
	if (m_selection.kind != element_kind::road)
	{
		select(element_ref(element_kind::road, road_id));
		return;
	}
	if (m_selection.id == road_id)
	{
		// The main road goes out: the last other road is the new main road
		if (m_more_roads.empty())
		{
			m_selection = element_ref();
		}
		else
		{
			m_selection = element_ref(element_kind::road, m_more_roads.back());
			m_more_roads.pop_back();
		}
	}
	else
	{
		const auto it = std::find(m_more_roads.begin(), m_more_roads.end(), road_id);
		if (it != m_more_roads.end())
		{
			m_more_roads.erase(it);
		}
		else
		{
			m_more_roads.push_back(m_selection.id);
			m_selection = element_ref(element_kind::road, road_id);
		}
	}
	emit selection_changed(m_selection);
}

std::vector<int> workspace::selected_roads() const
{
	std::vector<int> roads;
	if (m_selection.kind == element_kind::road)
	{
		roads = m_more_roads;
		roads.push_back(m_selection.id);
	}
	return roads;
}

void workspace::validate_selection()
{
	bool ok = true;
	switch (m_selection.kind)
	{
		case element_kind::node: ok = m_model.node(m_selection.id) != nullptr; break;
		case element_kind::road: ok = m_model.road(m_selection.id) != nullptr; break;
		case element_kind::light: ok = m_model.light(m_selection.id) != nullptr; break;
		case element_kind::sign: ok = m_model.sign(m_selection.id) != nullptr; break;
		case element_kind::object: ok = m_model.object(m_selection.id) != nullptr; break;
		case element_kind::vehicle: ok = m_sim.find_vehicle(m_selection.id) != nullptr; break;
		case element_kind::pedestrian: ok = m_sim.find_pedestrian(m_selection.id) != nullptr; break;
		default: break;
	}
	// The other selected roads that do not exist anymore go out
	const size_t more = m_more_roads.size();
	m_more_roads.erase(std::remove_if(m_more_roads.begin(), m_more_roads.end(),
		[this](int id) { return m_model.road(id) == nullptr; }), m_more_roads.end());
	if (!ok)
	{
		m_selection = element_ref();
		if (!m_more_roads.empty())
		{
			m_selection = element_ref(element_kind::road, m_more_roads.back());
			m_more_roads.pop_back();
		}
		emit selection_changed(m_selection);
	}
	else if (m_more_roads.size() != more)
	{
		emit selection_changed(m_selection);
	}
}

bool workspace::load(const QString &path, QString *error)
{
	if (!te_file::load(m_model, path, error))
	{
		return false;
	}
	m_path = path;
	m_dirty = false;
	m_selection = element_ref();
	m_net.build(m_model);
	m_sim.reset(&m_model, &m_net);
	emit network_rebuilt();
	emit changed();
	return true;
}

bool workspace::save(const QString &path, QString *error)
{
	if (!te_file::save(m_model, path, error))
	{
		return false;
	}
	m_path = path;
	m_dirty = false;
	emit changed();
	return true;
}
