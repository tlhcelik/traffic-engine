#include "te_file.h"
#include "world_model.h"
#include "xml_doc.h"
#include "logger.h"

#include <QFile>
#include <QSaveFile>
#include <QStringList>

namespace te_file
{
	const char *const extension = "te";

	namespace
	{
		const int format_version = 1;

		QString points_to_text(const std::vector<vec2> &pts)
		{
			QStringList parts;
			for (const vec2 &p : pts)
			{
				parts << QStringLiteral("%1,%2").arg(p.x, 0, 'f', 3).arg(p.y, 0, 'f', 3);
			}
			return parts.join(QLatin1Char(' '));
		}

		std::vector<vec2> text_to_points(const QString &text)
		{
			std::vector<vec2> out;
			const QStringList parts = text.split(QLatin1Char(' '), QString::SkipEmptyParts);
			for (const QString &p : parts)
			{
				const QStringList xy = p.split(QLatin1Char(','));
				if (xy.size() == 2)
				{
					out.push_back(vec2(xy[0].toDouble(), xy[1].toDouble()));
				}
			}
			return out;
		}

		QString ids_to_text(const std::vector<int> &ids)
		{
			QStringList parts;
			for (int id : ids)
			{
				parts << QString::number(id);
			}
			return parts.join(QLatin1Char(' '));
		}

		std::vector<int> text_to_ids(const QString &text)
		{
			std::vector<int> out;
			for (const QString &p : text.split(QLatin1Char(' '), QString::SkipEmptyParts))
			{
				bool ok = false;
				const int v = p.toInt(&ok);
				if (ok)
				{
					out.push_back(v);
				}
			}
			return out;
		}

		void write_doc(const world_model &m, xml_document &doc)
		{
			doc.create(QStringLiteral("traffic_engine_workspace"));
			xml_element root = doc.root();
			root.set_attribute(QStringLiteral("format"), format_version);
			root.set_attribute(QStringLiteral("program_version"), QStringLiteral(TE_VERSION));

			xml_element w = root.add_child(QStringLiteral("world"));
			w.set_attribute(QStringLiteral("name"), m.name);
			w.set_attribute(QStringLiteral("ground"), ground_name(m.ground));
			w.set_attribute(QStringLiteral("start_clock"), m.start_clock);
			w.set_attribute(QStringLiteral("geo_lat"), m.geo_lat);
			w.set_attribute(QStringLiteral("geo_lon"), m.geo_lon);
			if (!m.description.isEmpty())
			{
				w.set_child_text(QStringLiteral("description"), m.description);
			}

			xml_element nodes = root.add_child(QStringLiteral("nodes"));
			for (const auto &kv : m.nodes)
			{
				const node_data &n = kv.second;
				xml_element e = nodes.add_child(QStringLiteral("node"));
				e.set_attribute(QStringLiteral("id"), n.id);
				e.set_attribute(QStringLiteral("x"), n.pos.x);
				e.set_attribute(QStringLiteral("y"), n.pos.y);
				e.set_attribute(QStringLiteral("spawn"), n.spawn);
				e.set_attribute(QStringLiteral("vehicles_per_hour"), n.vehicles_per_hour);
				e.set_attribute(QStringLiteral("pedestrians_per_hour"), n.pedestrians_per_hour);
				e.set_attribute(QStringLiteral("share_bus"), n.share_bus);
				e.set_attribute(QStringLiteral("share_motorcycle"), n.share_motorcycle);
				e.set_attribute(QStringLiteral("daily_profile"), n.daily_profile);
			}

			xml_element roads = root.add_child(QStringLiteral("roads"));
			for (const auto &kv : m.roads)
			{
				const road_data &r = kv.second;
				xml_element e = roads.add_child(QStringLiteral("road"));
				e.set_attribute(QStringLiteral("id"), r.id);
				e.set_attribute(QStringLiteral("name"), r.name);
				e.set_attribute(QStringLiteral("a"), r.node_a);
				e.set_attribute(QStringLiteral("b"), r.node_b);
				e.set_attribute(QStringLiteral("lanes_forward"), r.lanes_forward);
				e.set_attribute(QStringLiteral("lanes_backward"), r.lanes_backward);
				e.set_attribute(QStringLiteral("lane_width"), r.lane_width);
				e.set_attribute(QStringLiteral("speed_limit"), r.speed_limit);
				e.set_attribute(QStringLiteral("overtaking"), r.overtaking);
				e.set_attribute(QStringLiteral("sidewalk_left"), r.sidewalk_left);
				e.set_attribute(QStringLiteral("sidewalk_right"), r.sidewalk_right);
				e.set_attribute(QStringLiteral("bike_lane"), r.bike_lane);
				e.set_attribute(QStringLiteral("ditch"), r.ditch);
				e.set_attribute(QStringLiteral("median"), r.median);
				e.set_attribute(QStringLiteral("guard_rail"), r.guard_rail);
				if (r.level != 0)
				{
					e.set_attribute(QStringLiteral("level"), r.level);
				}
				if (!r.shape.empty())
				{
					e.set_child_text(QStringLiteral("shape"), points_to_text(r.shape));
				}
			}

			xml_element lights = root.add_child(QStringLiteral("lights"));
			for (const auto &kv : m.lights)
			{
				const light_data &l = kv.second;
				xml_element e = lights.add_child(QStringLiteral("light"));
				e.set_attribute(QStringLiteral("id"), l.id);
				e.set_attribute(QStringLiteral("node"), l.node_id);
				e.set_attribute(QStringLiteral("algorithm"), light_algorithm_name(l.algorithm));
				e.set_attribute(QStringLiteral("offset"), l.offset);
				e.set_attribute(QStringLiteral("red_yellow"), l.red_yellow);
				for (const light_phase &ph : l.phases)
				{
					xml_element p = e.add_child(QStringLiteral("phase"));
					p.set_attribute(QStringLiteral("roads"), ids_to_text(ph.green_roads));
					p.set_attribute(QStringLiteral("green"), ph.green);
					p.set_attribute(QStringLiteral("yellow"), ph.yellow);
					p.set_attribute(QStringLiteral("all_red"), ph.all_red);
					p.set_attribute(QStringLiteral("min_green"), ph.min_green);
					p.set_attribute(QStringLiteral("max_green"), ph.max_green);
					p.set_attribute(QStringLiteral("pedestrian_only"), ph.pedestrian_only);
				}
			}

			xml_element signs = root.add_child(QStringLiteral("signs"));
			for (const auto &kv : m.signs)
			{
				const sign_data &s = kv.second;
				xml_element e = signs.add_child(QStringLiteral("sign"));
				e.set_attribute(QStringLiteral("id"), s.id);
				e.set_attribute(QStringLiteral("key"), s.key);
				e.set_attribute(QStringLiteral("road"), s.road_id);
				e.set_attribute(QStringLiteral("s"), s.s);
				e.set_attribute(QStringLiteral("dir"), s.dir);
				e.set_attribute(QStringLiteral("value"), s.value);
			}

			xml_element objects = root.add_child(QStringLiteral("objects"));
			for (const auto &kv : m.objects)
			{
				const object_data &o = kv.second;
				xml_element e = objects.add_child(QStringLiteral("object"));
				e.set_attribute(QStringLiteral("id"), o.id);
				e.set_attribute(QStringLiteral("kind"), object_kind_name(o.kind));
				e.set_attribute(QStringLiteral("road"), o.road_id);
				e.set_attribute(QStringLiteral("s"), o.s);
				e.set_attribute(QStringLiteral("dir"), o.dir);
				e.set_attribute(QStringLiteral("lane"), o.lane);
				e.set_attribute(QStringLiteral("length"), o.length);
				e.set_attribute(QStringLiteral("train_period"), o.train_period);
				e.set_attribute(QStringLiteral("closed_time"), o.closed_time);
				e.set_attribute(QStringLiteral("train_offset"), o.train_offset);
				e.set_attribute(QStringLiteral("dwell_time"), o.dwell_time);
				e.set_attribute(QStringLiteral("name"), o.name);
			}
		}

		bool read_doc(world_model &m, const xml_document &doc, const QString &source, QString *error)
		{
			const xml_element root = doc.root();
			if (root.name() != QLatin1String("traffic_engine_workspace"))
			{
				if (error)
				{
					*error = QStringLiteral("%1 is not a Traffic Engine workspace file.").arg(source);
				}
				return false;
			}
			const int fmt = root.attribute_int(QStringLiteral("format"), 0);
			if (fmt > format_version)
			{
				TE_WARN("FILE", QStringLiteral("%1 has a newer format (%2). Some data can be lost.").arg(source).arg(fmt));
			}
			m.clear();

			const xml_element w = root.first_child(QStringLiteral("world"));
			m.name = w.attribute(QStringLiteral("name"));
			m.ground = ground_from_name(w.attribute(QStringLiteral("ground"), QStringLiteral("grass")));
			m.start_clock = w.attribute_double(QStringLiteral("start_clock"), 8.0 * 3600.0);
			m.geo_lat = w.attribute_double(QStringLiteral("geo_lat"), 0.0);
			m.geo_lon = w.attribute_double(QStringLiteral("geo_lon"), 0.0);
			m.description = w.child_text(QStringLiteral("description"));

			for (const xml_element &e : root.first_child(QStringLiteral("nodes")).children(QStringLiteral("node")))
			{
				node_data n;
				n.id = e.attribute_int(QStringLiteral("id"), -1);
				if (n.id < 0 || m.nodes.count(n.id))
				{
					continue;
				}
				n.pos = vec2(e.attribute_double(QStringLiteral("x")), e.attribute_double(QStringLiteral("y")));
				n.spawn = e.attribute_bool(QStringLiteral("spawn"), true);
				n.vehicles_per_hour = e.attribute_double(QStringLiteral("vehicles_per_hour"), 600.0);
				n.pedestrians_per_hour = e.attribute_double(QStringLiteral("pedestrians_per_hour"), 60.0);
				n.share_bus = e.attribute_double(QStringLiteral("share_bus"), 0.05);
				n.share_motorcycle = e.attribute_double(QStringLiteral("share_motorcycle"), 0.15);
				n.daily_profile = e.attribute_bool(QStringLiteral("daily_profile"), true);
				m.nodes[n.id] = n;
				m.reserve_id(n.id);
			}

			for (const xml_element &e : root.first_child(QStringLiteral("roads")).children(QStringLiteral("road")))
			{
				road_data r;
				r.id = e.attribute_int(QStringLiteral("id"), -1);
				r.node_a = e.attribute_int(QStringLiteral("a"), -1);
				r.node_b = e.attribute_int(QStringLiteral("b"), -1);
				if (r.id < 0 || m.roads.count(r.id) || !m.node(r.node_a) || !m.node(r.node_b) || r.node_a == r.node_b)
				{
					TE_WARN("FILE", QStringLiteral("%1: road %2 has bad nodes and is not loaded.").arg(source).arg(r.id));
					continue;
				}
				r.name = e.attribute(QStringLiteral("name"));
				r.lanes_forward = qBound(0, e.attribute_int(QStringLiteral("lanes_forward"), 1), 8);
				r.lanes_backward = qBound(0, e.attribute_int(QStringLiteral("lanes_backward"), 1), 8);
				if (r.lanes_forward + r.lanes_backward == 0)
				{
					r.lanes_forward = 1;
				}
				r.lane_width = e.attribute_double(QStringLiteral("lane_width"), 3.5);
				r.speed_limit = e.attribute_double(QStringLiteral("speed_limit"), 50.0);
				r.overtaking = e.attribute_bool(QStringLiteral("overtaking"), true);
				r.sidewalk_left = e.attribute_bool(QStringLiteral("sidewalk_left"), true);
				r.sidewalk_right = e.attribute_bool(QStringLiteral("sidewalk_right"), true);
				r.bike_lane = e.attribute_bool(QStringLiteral("bike_lane"), false);
				r.ditch = e.attribute_bool(QStringLiteral("ditch"), false);
				r.median = e.attribute_bool(QStringLiteral("median"), false);
				r.guard_rail = e.attribute_bool(QStringLiteral("guard_rail"), false);
				r.level = std::max(-2, std::min(2, e.attribute_int(QStringLiteral("level"), 0)));
				r.shape = text_to_points(e.child_text(QStringLiteral("shape")));
				m.roads[r.id] = r;
				m.reserve_id(r.id);
			}

			for (const xml_element &e : root.first_child(QStringLiteral("lights")).children(QStringLiteral("light")))
			{
				light_data l;
				l.id = e.attribute_int(QStringLiteral("id"), -1);
				l.node_id = e.attribute_int(QStringLiteral("node"), -1);
				if (l.id < 0 || m.lights.count(l.id) || !m.node(l.node_id))
				{
					continue;
				}
				l.algorithm = light_algorithm_from_name(e.attribute(QStringLiteral("algorithm")));
				l.offset = e.attribute_double(QStringLiteral("offset"), 0.0);
				l.red_yellow = e.attribute_double(QStringLiteral("red_yellow"), 1.0);
				for (const xml_element &p : e.children(QStringLiteral("phase")))
				{
					light_phase ph;
					ph.green_roads = text_to_ids(p.attribute(QStringLiteral("roads")));
					ph.green = p.attribute_double(QStringLiteral("green"), 25.0);
					ph.yellow = p.attribute_double(QStringLiteral("yellow"), 3.0);
					ph.all_red = p.attribute_double(QStringLiteral("all_red"), 2.0);
					ph.min_green = p.attribute_double(QStringLiteral("min_green"), 8.0);
					ph.max_green = p.attribute_double(QStringLiteral("max_green"), 45.0);
					ph.pedestrian_only = p.attribute_bool(QStringLiteral("pedestrian_only"), false);
					l.phases.push_back(ph);
				}
				m.lights[l.id] = l;
				m.reserve_id(l.id);
			}
			m.repair_lights();

			for (const xml_element &e : root.first_child(QStringLiteral("signs")).children(QStringLiteral("sign")))
			{
				sign_data s;
				s.id = e.attribute_int(QStringLiteral("id"), -1);
				s.road_id = e.attribute_int(QStringLiteral("road"), -1);
				if (s.id < 0 || m.signs.count(s.id) || !m.road(s.road_id))
				{
					continue;
				}
				s.key = e.attribute(QStringLiteral("key"));
				s.s = e.attribute_double(QStringLiteral("s"), 0.0);
				s.dir = e.attribute_int(QStringLiteral("dir"), 1) >= 0 ? 1 : -1;
				s.value = e.attribute_int(QStringLiteral("value"), 0);
				m.signs[s.id] = s;
				m.reserve_id(s.id);
			}

			for (const xml_element &e : root.first_child(QStringLiteral("objects")).children(QStringLiteral("object")))
			{
				object_data o;
				o.id = e.attribute_int(QStringLiteral("id"), -1);
				o.road_id = e.attribute_int(QStringLiteral("road"), -1);
				if (o.id < 0 || m.objects.count(o.id) || !m.road(o.road_id))
				{
					continue;
				}
				o.kind = object_kind_from_name(e.attribute(QStringLiteral("kind")));
				o.s = e.attribute_double(QStringLiteral("s"), 0.0);
				o.dir = e.attribute_int(QStringLiteral("dir"), 1);
				o.lane = e.attribute_int(QStringLiteral("lane"), 0);
				o.length = e.attribute_double(QStringLiteral("length"), 12.0);
				o.train_period = e.attribute_double(QStringLiteral("train_period"), 300.0);
				o.closed_time = e.attribute_double(QStringLiteral("closed_time"), 40.0);
				o.train_offset = e.attribute_double(QStringLiteral("train_offset"), 0.0);
				o.dwell_time = e.attribute_double(QStringLiteral("dwell_time"), 20.0);
				o.name = e.attribute(QStringLiteral("name"));
				m.objects[o.id] = o;
				m.reserve_id(o.id);
			}
			m.touch_network();
			return true;
		}
	}

	QByteArray to_bytes(const world_model &model)
	{
		xml_document doc;
		write_doc(model, doc);
		return doc.to_bytes();
	}

	bool save(const world_model &model, const QString &path, QString *error)
	{
		xml_document doc;
		write_doc(model, doc);
		if (!doc.save(path, error))
		{
			return false;
		}
		logger::instance().file_event(QStringLiteral("Workspace saved"), path);
		return true;
	}

	bool from_bytes(world_model &model, const QByteArray &data, const QString &source, QString *error)
	{
		xml_document doc;
		if (!doc.load_from_memory(data, source, error))
		{
			return false;
		}
		return read_doc(model, doc, source, error);
	}

	bool load(world_model &model, const QString &path, QString *error)
	{
		xml_document doc;
		if (!doc.load(path, error))
		{
			return false;
		}
		if (!read_doc(model, doc, path, error))
		{
			return false;
		}
		logger::instance().file_event(QStringLiteral("Workspace opened"), path);
		return true;
	}
}
