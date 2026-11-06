#include "json_export.h"
#include "geo.h"
#include "road_network.h"
#include "stats_collector.h"
#include "workspace.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QTime>

#include <cmath>

namespace
{
	// "red light" -> "red_light" (the same form as the other keys of the API).
	QString cause_key(wait_cause c)
	{
		return QString::fromLatin1(wait_cause_name(c)).replace(QLatin1Char(' '), QLatin1Char('_'));
	}

	// Rounds v to the given number of decimals.
	double rounded(double v, int decimals)
	{
		const double f = std::pow(10.0, decimals);
		return std::round(v * f) / f;
	}
}

namespace json_export
{
	QJsonObject program_object()
	{
		QJsonObject program;
		program.insert(QStringLiteral("name"), QStringLiteral("Traffic Engine"));
		program.insert(QStringLiteral("version"), QStringLiteral(TE_VERSION));
		program.insert(QStringLiteral("build"), QStringLiteral(TE_BUILD_CONFIG));
		program.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
		program.insert(QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate));
		return program;
	}

	QJsonObject workspace_object(const workspace &ws)
	{
		const world_model &m = ws.model();
		QJsonObject wsj;
		wsj.insert(QStringLiteral("name"), ws.display_name());
		wsj.insert(QStringLiteral("file"), ws.file_path());
		wsj.insert(QStringLiteral("ground"), ground_name(m.ground));
		wsj.insert(QStringLiteral("nodes"), static_cast<int>(m.nodes.size()));
		wsj.insert(QStringLiteral("roads"), static_cast<int>(m.roads.size()));
		wsj.insert(QStringLiteral("road_km"), m.total_road_km());
		wsj.insert(QStringLiteral("lane_km"), ws.network().total_lane_km);
		wsj.insert(QStringLiteral("traffic_lights"), static_cast<int>(m.lights.size()));
		wsj.insert(QStringLiteral("signs"), static_cast<int>(m.signs.size()));
		wsj.insert(QStringLiteral("objects"), static_cast<int>(m.objects.size()));
		if (m.has_geo())
		{
			QJsonObject origin;
			origin.insert(QStringLiteral("lat"), m.geo_lat);
			origin.insert(QStringLiteral("lon"), m.geo_lon);
			wsj.insert(QStringLiteral("origin"), origin);
		}
		return wsj;
	}

	QJsonObject simulation_object(const workspace &ws)
	{
		const simulation &sim = ws.sim();
		QJsonObject simj;
		simj.insert(QStringLiteral("clock"), QTime(0, 0).addSecs(static_cast<int>(sim.clock())).toString(QStringLiteral("HH:mm:ss")));
		simj.insert(QStringLiteral("time_s"), sim.time());
		simj.insert(QStringLiteral("speed"), ws.speed());
		simj.insert(QStringLiteral("running"), ws.running());
		simj.insert(QStringLiteral("editor_mode"), ws.editor_mode());
		simj.insert(QStringLiteral("traffic_density_percent"), sim.vehicle_demand_factor() * 100.0);
		simj.insert(QStringLiteral("pedestrian_density_percent"), sim.pedestrian_demand_factor() * 100.0);
		simj.insert(QStringLiteral("vehicles"), static_cast<int>(sim.vehicles().size()));
		simj.insert(QStringLiteral("pedestrians"), static_cast<int>(sim.pedestrians().size()));
		simj.insert(QStringLiteral("completed_trips"), static_cast<double>(sim.finished_vehicles()));
		simj.insert(QStringLiteral("gridlock_removed"), static_cast<double>(sim.gridlock_removed()));
		return simj;
	}

	QJsonObject statistics_object(const workspace &ws)
	{
		const simulation &sim = ws.sim();
		QJsonObject stats;
		for (int i = 0; i < stats_collector::metric_count; ++i)
		{
			QJsonObject v;
			v.insert(QStringLiteral("value"), sim.stats().last(static_cast<stats_collector::metric>(i)));
			v.insert(QStringLiteral("unit"), stats_collector::metric_unit(i));
			stats.insert(stats_collector::metric_key(i), v);
		}
		return stats;
	}

	QJsonArray lights_array(const workspace &ws)
	{
		const world_model &m = ws.model();
		const simulation &sim = ws.sim();
		static const char *stage_names[] = { "red_yellow", "green", "yellow", "all_red" };
		static const char *colors[] = { "off", "red", "red_yellow", "green", "yellow" };
		QJsonArray lights;
		for (const auto &kv : m.lights)
		{
			const light_runtime *lr = sim.light_state(kv.first);
			QJsonObject l;
			l.insert(QStringLiteral("id"), kv.first);
			l.insert(QStringLiteral("node"), kv.second.node_id);
			l.insert(QStringLiteral("algorithm"), light_algorithm_name(kv.second.algorithm));
			l.insert(QStringLiteral("phases"), static_cast<int>(kv.second.phases.size()));
			if (lr)
			{
				l.insert(QStringLiteral("phase"), lr->phase + 1);
				l.insert(QStringLiteral("stage"), QString::fromLatin1(stage_names[qBound(0, lr->stage, 3)]));
				l.insert(QStringLiteral("stage_remaining_s"), lr->stage_left);
				l.insert(QStringLiteral("external"), lr->external);
			}
			QJsonArray approaches;
			for (int rid : m.roads_at_node(kv.second.node_id))
			{
				QJsonObject a;
				a.insert(QStringLiteral("road"), rid);
				const road_data *rd = m.road(rid);
				if (rd && !rd->name.isEmpty())
				{
					a.insert(QStringLiteral("name"), rd->name);
				}
				a.insert(QStringLiteral("signal"), QString::fromLatin1(colors[static_cast<int>(sim.signal_for(kv.second.node_id, rid))]));
				a.insert(QStringLiteral("remaining_s"), sim.signal_remaining(kv.second.node_id, rid));
				a.insert(QStringLiteral("waiting"), sim.waiting_at(kv.second.node_id, rid));
				approaches.append(a);
			}
			l.insert(QStringLiteral("approaches"), approaches);
			lights.append(l);
		}
		return lights;
	}

	QJsonArray vehicles_array(const workspace &ws)
	{
		const world_model &m = ws.model();
		const geo_projection proj = m.has_geo() ? geo_projection(m.geo_lat, m.geo_lon) : geo_projection();
		QJsonArray vehicles;
		for (const sim_vehicle &v : ws.sim().vehicles())
		{
			QJsonObject o;
			o.insert(QStringLiteral("id"), v.id);
			o.insert(QStringLiteral("type"), v.p->name.toLower());
			o.insert(QStringLiteral("x"), v.pos.x);
			o.insert(QStringLiteral("y"), v.pos.y);
			if (proj.valid())
			{
				double lat = 0.0;
				double lon = 0.0;
				proj.to_geo(v.pos, &lat, &lon);
				o.insert(QStringLiteral("lat"), lat);
				o.insert(QStringLiteral("lon"), lon);
			}
			o.insert(QStringLiteral("heading_deg"), v.dir.angle() * 180.0 / M_PI);
			o.insert(QStringLiteral("speed_kmh"), v.v * 3.6);
			o.insert(QStringLiteral("acceleration"), v.a);
			o.insert(QStringLiteral("fuel_l"), v.fuel_l);
			o.insert(QStringLiteral("co2_kg"), v.co2_kg);
			o.insert(QStringLiteral("passengers"), v.passengers);
			o.insert(QStringLiteral("stops"), v.stops);
			o.insert(QStringLiteral("origin_node"), v.origin_node);
			o.insert(QStringLiteral("destination_node"), v.dest_node);
			vehicles.append(o);
		}
		return vehicles;
	}

	QJsonObject congestion_object(const workspace &ws)
	{
		const world_model &m = ws.model();
		const simulation &sim = ws.sim();
		QJsonArray junctions;
		for (const junction_status &j : sim.congestion())
		{
			QJsonObject o;
			o.insert(QStringLiteral("node"), j.node_id);
			o.insert(QStringLiteral("signalized"), j.signalized);
			o.insert(QStringLiteral("los"), QString(QLatin1Char(j.los)));
			o.insert(QStringLiteral("delay_s"), rounded(j.delay, 1));
			o.insert(QStringLiteral("flow_veh_h"), rounded(j.flow, 0));
			o.insert(QStringLiteral("queue_m"), rounded(j.queue_m, 1));
			o.insert(QStringLiteral("waiting"), j.waiting);
			o.insert(QStringLiteral("spillback"), j.spillback);
			o.insert(QStringLiteral("main_cause"), cause_key(j.cause));
			o.insert(QStringLiteral("main_cause_share"), rounded(j.cause_share, 2));
			if (j.blocking_node >= 0)
			{
				o.insert(QStringLiteral("blocking_node"), j.blocking_node);
			}
			QJsonArray approaches;
			for (const approach_status &a : j.approaches)
			{
				QJsonObject ao;
				ao.insert(QStringLiteral("road"), a.road_id);
				const road_data *rd = m.road(a.road_id);
				if (rd && !rd->name.isEmpty())
				{
					ao.insert(QStringLiteral("name"), rd->name);
				}
				ao.insert(QStringLiteral("los"), QString(QLatin1Char(a.spillback ? 'F' : simulation::los_letter(a.delay, j.signalized))));
				ao.insert(QStringLiteral("delay_s"), rounded(a.delay, 1));
				ao.insert(QStringLiteral("queue_m"), rounded(a.queue_m, 1));
				ao.insert(QStringLiteral("max_queue_m"), rounded(a.max_queue_m, 1));
				ao.insert(QStringLiteral("queue_vehicles"), a.queue_veh);
				ao.insert(QStringLiteral("length_m"), rounded(a.length, 1));
				ao.insert(QStringLiteral("passages"), a.passages);
				ao.insert(QStringLiteral("spillback"), a.spillback);
				ao.insert(QStringLiteral("main_cause"), cause_key(a.cause));
				approaches.append(ao);
			}
			o.insert(QStringLiteral("approaches"), approaches);
			junctions.append(o);
		}
		QJsonObject c;
		c.insert(QStringLiteral("window_s"), sim.congestion_window());
		c.insert(QStringLiteral("junctions"), junctions);
		return c;
	}

	QByteArray workspace_report(const workspace &ws, bool include_vehicles)
	{
		QJsonObject root;
		root.insert(QStringLiteral("program"), program_object());
		root.insert(QStringLiteral("workspace"), workspace_object(ws));
		root.insert(QStringLiteral("simulation"), simulation_object(ws));
		root.insert(QStringLiteral("statistics"), statistics_object(ws));
		root.insert(QStringLiteral("traffic_lights"), lights_array(ws));
		root.insert(QStringLiteral("congestion"), congestion_object(ws));
		if (include_vehicles)
		{
			root.insert(QStringLiteral("vehicles"), vehicles_array(ws));
		}
		return QJsonDocument(root).toJson(QJsonDocument::Indented);
	}
}
