#include "world_properties.h"
#include "i18n.h"
#include "sign_catalog.h"

#include <QRegularExpression>

#include <cmath>

namespace
{
	property_def make(property_def::type_t type, const QString &key, const QString &label, const QVariant &value,
		const QString &group, const QString &unit = QString())
	{
		property_def p;
		p.type = type;
		p.key = key;
		p.label = label;
		p.value = value;
		p.group = group;
		p.unit = unit;
		return p;
	}

	property_def info(const QString &key, const QString &label, const QVariant &value, const QString &group, const QString &unit = QString())
	{
		return make(property_def::t_info, key, label, value, group, unit);
	}

	property_def boolean(const QString &key, const QString &label, bool value, const QString &group)
	{
		return make(property_def::t_bool, key, label, value, group);
	}

	property_def integer(const QString &key, const QString &label, int value, int min, int max, const QString &group, const QString &unit = QString())
	{
		property_def p = make(property_def::t_int, key, label, value, group, unit);
		p.min = min;
		p.max = max;
		return p;
	}

	property_def real(const QString &key, const QString &label, double value, double min, double max, double step,
		int decimals, const QString &group, const QString &unit = QString())
	{
		property_def p = make(property_def::t_double, key, label, value, group, unit);
		p.min = min;
		p.max = max;
		p.step = step;
		p.decimals = decimals;
		return p;
	}

	property_def text(const QString &key, const QString &label, const QString &value, const QString &group)
	{
		return make(property_def::t_string, key, label, value, group);
	}

	property_def choice(const QString &key, const QString &label, int index, const QStringList &options, const QString &group)
	{
		property_def p = make(property_def::t_enum, key, label, index, group);
		p.options = options;
		return p;
	}

	QStringList ground_options()
	{
		return { tx("Grass"), tx("Soil"), tx("Sand"), tx("Snow"), tx("Concrete"), tx("Asphalt") };
	}

	QStringList dir_options()
	{
		return { tx("Traffic from A to B"), tx("Traffic from B to A") };
	}

	QStringList dir3_options()
	{
		return { tx("Both directions"), tx("Traffic from A to B"), tx("Traffic from B to A") };
	}

	// Road levels -2 .. 2 (index = level + 2)
	QStringList level_options()
	{
		return { tx("Lower tunnel (level -2)"), tx("Tunnel or underpass (level -1)"), tx("Ground (level 0)"),
			tx("Bridge or overpass (level 1)"), tx("Upper bridge (level 2)") };
	}

	int dir3_index(int dir) { return dir == 0 ? 0 : (dir > 0 ? 1 : 2); }
	int dir3_value(int index) { return index == 0 ? 0 : (index == 1 ? 1 : -1); }

	// Helpers for set(): assign and report a change
	template <typename T>
	bool assign(T &field, const T &value)
	{
		if (field == value)
		{
			return false;
		}
		field = value;
		return true;
	}

	bool assign_d(double &field, double value)
	{
		if (std::abs(field - value) < 1e-9)
		{
			return false;
		}
		field = value;
		return true;
	}

	QString node_type(const world_model &m, int node_id)
	{
		const size_t n = m.roads_at_node(node_id).size();
		if (n == 0) { return tx("Free node"); }
		if (n == 1) { return tx("Dead end (traffic source and destination)"); }
		if (n == 2) { return tx("Road connection"); }
		return tx("Junction with %1 roads").arg(n);
	}
}

namespace world_properties
{
	QList<property_def> list_roads(const world_model &m, const std::vector<int> &roads)
	{
		QList<property_def> out;
		if (roads.empty())
		{
			return out;
		}
		const QString g = QStringLiteral("Selected roads");
		double total = 0.0;
		QStringList ids;
		std::vector<QList<property_def>> each;
		for (int id : roads)
		{
			const road_data *r = m.road(id);
			if (!r)
			{
				continue;
			}
			total += m.road_centerline(*r).length();
			ids << QString::number(id);
			each.push_back(list(m, element_ref(element_kind::road, id)));
		}
		out << info(QStringLiteral("count"), QStringLiteral("Roads"), ids.size(), g)
			<< info(QStringLiteral("ids"), QStringLiteral("IDs"), ids.join(QStringLiteral(", ")), g)
			<< info(QStringLiteral("length"), QStringLiteral("Total length"), QString::number(total, 'f', 1), g, QStringLiteral("m"));
		for (property_def d : list(m, element_ref(element_kind::road, roads.back())))
		{
			if (d.type == property_def::t_info)
			{
				continue; // ID, nodes and length of one road
			}
			for (const QList<property_def> &defs : each)
			{
				for (const property_def &o : defs)
				{
					if (o.key == d.key && o.value != d.value)
					{
						d.mixed = true;
					}
				}
			}
			out << d;
		}
		return out;
	}

	bool set_roads(world_model &m, const std::vector<int> &roads, const QString &key, const QVariant &value, property_change *change)
	{
		bool any = false;
		*change = property_change::change_none;
		for (int id : roads)
		{
			property_change c = property_change::change_none;
			if (set(m, element_ref(element_kind::road, id), key, value, &c))
			{
				any = true;
				*change = std::max(*change, c);
			}
		}
		return any;
	}

	QString title(const world_model &m, const element_ref &ref, bool translated)
	{
		// English for the log, else the language of the user interface
		auto t = [translated](const char *text) { return translated ? tx(text) : QString::fromUtf8(text); };
		switch (ref.kind)
		{
			case element_kind::none: return t("Workspace");
			case element_kind::node: return t("Node %1").arg(ref.id);
			case element_kind::road:
			{
				const road_data *r = m.road(ref.id);
				return (r && !r->name.isEmpty()) ? t("Road %1 - %2").arg(ref.id).arg(r->name) : t("Road %1").arg(ref.id);
			}
			case element_kind::light: return t("Traffic light %1").arg(ref.id);
			case element_kind::sign:
			{
				const sign_data *s = m.sign(ref.id);
				const sign_def *d = s ? sign_catalog::instance().find(s->key) : nullptr;
				return d ? t("Sign %1 - %2").arg(ref.id).arg(translated ? d->display_name() : d->name) : t("Sign %1").arg(ref.id);
			}
			case element_kind::object:
			{
				const object_data *o = m.object(ref.id);
				static const char *names[] = { "Cones", "Barrier", "Road closure", "Level crossing", "Bus stop" };
				return o ? QStringLiteral("%1 %2").arg(t(names[static_cast<int>(o->kind)])).arg(ref.id) : t("Object %1").arg(ref.id);
			}
			case element_kind::vehicle: return t("Vehicle %1").arg(ref.id);
			case element_kind::pedestrian: return t("Pedestrian %1").arg(ref.id);
		}
		return QString();
	}

	QList<property_def> list(const world_model &m, const element_ref &ref)
	{
		QList<property_def> out;
		switch (ref.kind)
		{
			case element_kind::none:
			{
				const QString g = QStringLiteral("Workspace");
				out << text(QStringLiteral("name"), QStringLiteral("Name"), m.name, g)
					<< text(QStringLiteral("description"), QStringLiteral("Description"), m.description, g)
					<< choice(QStringLiteral("ground"), QStringLiteral("Ground material"), static_cast<int>(m.ground), ground_options(), g)
					<< real(QStringLiteral("start_clock"), QStringLiteral("Start clock"), m.start_clock / 3600.0, 0.0, 23.99, 0.25, 2, g, QStringLiteral("h"));
				// Geographic origin: the map background (0, 0 = no map)
				const QString gm = QStringLiteral("Map (origin of the world)");
				out << real(QStringLiteral("geo_lat"), QStringLiteral("Latitude"), m.geo_lat, -85.0, 85.0, 0.0001, 6, gm, QStringLiteral("deg"))
					<< real(QStringLiteral("geo_lon"), QStringLiteral("Longitude"), m.geo_lon, -180.0, 180.0, 0.0001, 6, gm, QStringLiteral("deg"));
				const QString gi = QStringLiteral("Contents");
				out << info(QStringLiteral("nodes"), QStringLiteral("Nodes"), static_cast<int>(m.nodes.size()), gi)
					<< info(QStringLiteral("roads"), QStringLiteral("Roads"), static_cast<int>(m.roads.size()), gi)
					<< info(QStringLiteral("road_km"), QStringLiteral("Road length"), QString::number(m.total_road_km(), 'f', 2), gi, QStringLiteral("km"))
					<< info(QStringLiteral("lights"), QStringLiteral("Traffic lights"), static_cast<int>(m.lights.size()), gi)
					<< info(QStringLiteral("signs"), QStringLiteral("Signs"), static_cast<int>(m.signs.size()), gi)
					<< info(QStringLiteral("objects"), QStringLiteral("Other objects"), static_cast<int>(m.objects.size()), gi);
				break;
			}
			case element_kind::node:
			{
				const node_data *n = m.node(ref.id);
				if (!n)
				{
					break;
				}
				const QString g = QStringLiteral("Node");
				out << info(QStringLiteral("id"), QStringLiteral("ID"), n->id, g)
					<< info(QStringLiteral("type"), QStringLiteral("Type"), node_type(m, n->id), g)
					<< real(QStringLiteral("x"), QStringLiteral("Position X"), n->pos.x, -1e6, 1e6, 1.0, 2, g, QStringLiteral("m"))
					<< real(QStringLiteral("y"), QStringLiteral("Position Y"), n->pos.y, -1e6, 1e6, 1.0, 2, g, QStringLiteral("m"));
				if (m.roads_at_node(n->id).size() == 1)
				{
					const QString gd = QStringLiteral("Traffic demand (peak hour)");
					out << boolean(QStringLiteral("spawn"), QStringLiteral("Make traffic"), n->spawn, gd)
						<< real(QStringLiteral("vehicles_per_hour"), QStringLiteral("Vehicles"), n->vehicles_per_hour, 0.0, 6000.0, 50.0, 0, gd, QStringLiteral("veh/h"))
						<< real(QStringLiteral("pedestrians_per_hour"), QStringLiteral("Pedestrians"), n->pedestrians_per_hour, 0.0, 3000.0, 10.0, 0, gd, QStringLiteral("ped/h"))
						<< real(QStringLiteral("share_bus"), QStringLiteral("Share of buses"), n->share_bus, 0.0, 1.0, 0.01, 2, gd)
						<< real(QStringLiteral("share_motorcycle"), QStringLiteral("Share of motorcycles"), n->share_motorcycle, 0.0, 1.0, 0.01, 2, gd)
						<< boolean(QStringLiteral("daily_profile"), QStringLiteral("Daily profile (peaks at 08:00 and 18:00)"), n->daily_profile, gd);
				}
				break;
			}
			case element_kind::road:
			{
				const road_data *r = m.road(ref.id);
				if (!r)
				{
					break;
				}
				const QString g = QStringLiteral("Road");
				out << info(QStringLiteral("id"), QStringLiteral("ID"), r->id, g)
					<< info(QStringLiteral("nodes"), QStringLiteral("Nodes (A -> B)"), QStringLiteral("%1 -> %2").arg(r->node_a).arg(r->node_b), g)
					<< info(QStringLiteral("length"), QStringLiteral("Length"), QString::number(m.road_centerline(*r).length(), 'f', 1), g, QStringLiteral("m"))
					<< text(QStringLiteral("name"), QStringLiteral("Name"), r->name, g)
					<< choice(QStringLiteral("level"), QStringLiteral("Level"), std::max(-2, std::min(2, r->level)) + 2, level_options(), g);
				const QString gl = QStringLiteral("Lanes");
				out << integer(QStringLiteral("lanes_forward"), QStringLiteral("Lanes A -> B"), r->lanes_forward, 0, 6, gl)
					<< integer(QStringLiteral("lanes_backward"), QStringLiteral("Lanes B -> A (0 = one-way)"), r->lanes_backward, 0, 6, gl)
					<< real(QStringLiteral("lane_width"), QStringLiteral("Lane width"), r->lane_width, 2.5, 5.0, 0.25, 2, gl, QStringLiteral("m"))
					<< real(QStringLiteral("speed_limit"), QStringLiteral("Speed limit"), r->speed_limit, 10.0, 140.0, 10.0, 0, gl, QStringLiteral("km/h"))
					<< boolean(QStringLiteral("overtaking"), QStringLiteral("Overtaking allowed (dashed lines)"), r->overtaking, gl)
					<< boolean(QStringLiteral("median"), QStringLiteral("Median strip"), r->median, gl);
				const QString gs = QStringLiteral("Road side");
				out << boolean(QStringLiteral("sidewalk_left"), QStringLiteral("Sidewalk on the left"), r->sidewalk_left, gs)
					<< boolean(QStringLiteral("sidewalk_right"), QStringLiteral("Sidewalk on the right"), r->sidewalk_right, gs)
					<< boolean(QStringLiteral("bike_lane"), QStringLiteral("Bicycle lanes"), r->bike_lane, gs)
					<< boolean(QStringLiteral("ditch"), QStringLiteral("Ditches"), r->ditch, gs)
					<< boolean(QStringLiteral("guard_rail"), QStringLiteral("Guard rails"), r->guard_rail, gs);
				break;
			}
			case element_kind::light:
			{
				const light_data *l = m.light(ref.id);
				if (!l)
				{
					break;
				}
				const QString g = QStringLiteral("Traffic light");
				double cycle = 0.0;
				for (const light_phase &ph : l->phases)
				{
					cycle += ph.green + ph.yellow + ph.all_red + l->red_yellow;
				}
				out << info(QStringLiteral("id"), QStringLiteral("ID"), l->id, g)
					<< info(QStringLiteral("node"), QStringLiteral("Node"), l->node_id, g)
					<< choice(QStringLiteral("algorithm"), QStringLiteral("Algorithm"), static_cast<int>(l->algorithm),
						{ tx("Fixed time"), tx("Actuated (vehicle detection)"), tx("External (API)") }, g)
					<< real(QStringLiteral("offset"), QStringLiteral("Start offset"), l->offset, 0.0, 600.0, 1.0, 0, g, QStringLiteral("s"))
					<< real(QStringLiteral("red_yellow"), QStringLiteral("Red and yellow time"), l->red_yellow, 0.0, 3.0, 0.5, 1, g, QStringLiteral("s"))
					<< info(QStringLiteral("cycle"), QStringLiteral("Cycle time"), QString::number(cycle, 'f', 0), g, QStringLiteral("s"));
				for (int k = 0; k < static_cast<int>(l->phases.size()); ++k)
				{
					const light_phase &ph = l->phases[k];
					const QString gp = QStringLiteral("Phase %1").arg(k + 1);
					QStringList roads;
					for (int rid : ph.green_roads)
					{
						const road_data *r = m.road(rid);
						roads << ((r && !r->name.isEmpty()) ? r->name : tx("road %1").arg(rid));
					}
					const QString pre = QStringLiteral("phase_%1_").arg(k);
					out << info(pre + QStringLiteral("roads"), QStringLiteral("Green for"), ph.pedestrian_only ? tx("Pedestrians only") : roads.join(QStringLiteral(", ")), gp)
						<< real(pre + QStringLiteral("green"), QStringLiteral("Green"), ph.green, 3.0, 300.0, 1.0, 0, gp, QStringLiteral("s"))
						<< real(pre + QStringLiteral("yellow"), QStringLiteral("Yellow"), ph.yellow, 0.0, 6.0, 0.5, 1, gp, QStringLiteral("s"))
						<< real(pre + QStringLiteral("all_red"), QStringLiteral("All red"), ph.all_red, 0.0, 10.0, 0.5, 1, gp, QStringLiteral("s"));
					if (l->algorithm == light_algorithm::actuated)
					{
						out << real(pre + QStringLiteral("min_green"), QStringLiteral("Minimum green"), ph.min_green, 3.0, 120.0, 1.0, 0, gp, QStringLiteral("s"))
							<< real(pre + QStringLiteral("max_green"), QStringLiteral("Maximum green"), ph.max_green, 5.0, 300.0, 1.0, 0, gp, QStringLiteral("s"));
					}
				}
				break;
			}
			case element_kind::sign:
			{
				const sign_data *s = m.sign(ref.id);
				if (!s)
				{
					break;
				}
				const sign_catalog &cat = sign_catalog::instance();
				const sign_def *d = cat.find(s->key);
				const QString g = QStringLiteral("Sign");
				QStringList names;
				int index = 0;
				for (int i = 0; i < cat.all().size(); ++i)
				{
					const sign_def &x = cat.all().at(i);
					names << x.label();
					if (x.key == s->key)
					{
						index = i;
					}
				}
				out << info(QStringLiteral("id"), QStringLiteral("ID"), s->id, g)
					<< choice(QStringLiteral("key"), QStringLiteral("Sign"), index, names, g)
					<< info(QStringLiteral("code"), QStringLiteral("Official code"), d && !d->code.isEmpty() ? d->code : tx("(not confirmed yet)"), g)
					<< info(QStringLiteral("name_tr"), QStringLiteral("Turkish name"), d ? d->name_tr : QString(), g)
					<< info(QStringLiteral("effect"), QStringLiteral("Effect"), d ? sign_catalog::effect_text(d->effect) : QString(), g);
				if (d && (d->symbol == QLatin1String("value") || d->symbol == QLatin1String("end_value")))
				{
					out << integer(QStringLiteral("value"), QStringLiteral("Value"), s->value > 0 ? s->value : d->default_value, 5, 140, g, QStringLiteral("km/h"));
				}
				const road_data *r = m.road(s->road_id);
				const double len = r ? m.road_centerline(*r).length() : 1000.0;
				out << choice(QStringLiteral("dir"), QStringLiteral("For"), s->dir > 0 ? 0 : 1, dir_options(), g)
					<< real(QStringLiteral("s"), QStringLiteral("Position on the road (from A)"), s->s, 0.0, len, 1.0, 1, g, QStringLiteral("m"));
				break;
			}
			case element_kind::object:
			{
				const object_data *o = m.object(ref.id);
				if (!o)
				{
					break;
				}
				const QString g = title(m, ref, false); // the panel translates the group (tx_message)
				const road_data *r = m.road(o->road_id);
				const double len = r ? m.road_centerline(*r).length() : 1000.0;
				out << info(QStringLiteral("id"), QStringLiteral("ID"), o->id, g)
					<< info(QStringLiteral("road"), QStringLiteral("Road"), o->road_id, g)
					<< real(QStringLiteral("s"), QStringLiteral("Position on the road (from A)"), o->s, 0.0, len, 1.0, 1, g, QStringLiteral("m"));
				switch (o->kind)
				{
					case object_kind::cone:
					case object_kind::barrier:
						out << choice(QStringLiteral("dir"), QStringLiteral("Direction"), dir3_index(o->dir), dir3_options(), g)
							<< integer(QStringLiteral("lane"), QStringLiteral("Blocked lane (0 = right, -1 = all)"), o->lane, -1, 5, g)
							<< real(QStringLiteral("length"), QStringLiteral("Blocked length"), o->length, 2.0, 300.0, 1.0, 0, g, QStringLiteral("m"));
						break;
					case object_kind::road_closure:
						out << choice(QStringLiteral("dir"), QStringLiteral("Closed for"), dir3_index(o->dir), dir3_options(), g);
						break;
					case object_kind::level_crossing:
						out << real(QStringLiteral("train_period"), QStringLiteral("Time between trains"), o->train_period, 60.0, 7200.0, 10.0, 0, g, QStringLiteral("s"))
							<< real(QStringLiteral("closed_time"), QStringLiteral("Barrier down time"), o->closed_time, 10.0, 600.0, 5.0, 0, g, QStringLiteral("s"))
							<< real(QStringLiteral("train_offset"), QStringLiteral("Time offset"), o->train_offset, 0.0, 7200.0, 10.0, 0, g, QStringLiteral("s"));
						break;
					case object_kind::bus_stop:
						out << text(QStringLiteral("name"), QStringLiteral("Name"), o->name, g)
							<< choice(QStringLiteral("dir"), QStringLiteral("Side (traffic)"), o->dir > 0 ? 0 : 1, dir_options(), g)
							<< real(QStringLiteral("dwell_time"), QStringLiteral("Stop time of a bus"), o->dwell_time, 5.0, 300.0, 5.0, 0, g, QStringLiteral("s"));
						break;
				}
				break;
			}
			default:
				break;
		}
		return out;
	}

	bool set(world_model &m, const element_ref &ref, const QString &key, const QVariant &value, property_change *change)
	{
		property_change ch = property_change::change_none;
		bool changed = false;
		switch (ref.kind)
		{
			case element_kind::none:
				if (key == QLatin1String("name")) { changed = assign(m.name, value.toString()); }
				else if (key == QLatin1String("description")) { changed = assign(m.description, value.toString()); }
				else if (key == QLatin1String("ground")) { changed = assign(m.ground, static_cast<ground_material>(value.toInt())); }
				else if (key == QLatin1String("start_clock")) { changed = assign_d(m.start_clock, value.toDouble() * 3600.0); }
				else if (key == QLatin1String("geo_lat")) { changed = assign_d(m.geo_lat, value.toDouble()); }
				else if (key == QLatin1String("geo_lon")) { changed = assign_d(m.geo_lon, value.toDouble()); }
				break;
			case element_kind::node:
			{
				node_data *n = m.node(ref.id);
				if (!n) { break; }
				if (key == QLatin1String("x")) { changed = assign_d(n->pos.x, value.toDouble()); ch = property_change::change_network; }
				else if (key == QLatin1String("y")) { changed = assign_d(n->pos.y, value.toDouble()); ch = property_change::change_network; }
				else if (key == QLatin1String("spawn")) { changed = assign(n->spawn, value.toBool()); }
				else if (key == QLatin1String("vehicles_per_hour")) { changed = assign_d(n->vehicles_per_hour, value.toDouble()); }
				else if (key == QLatin1String("pedestrians_per_hour")) { changed = assign_d(n->pedestrians_per_hour, value.toDouble()); }
				else if (key == QLatin1String("share_bus")) { changed = assign_d(n->share_bus, value.toDouble()); }
				else if (key == QLatin1String("share_motorcycle")) { changed = assign_d(n->share_motorcycle, value.toDouble()); }
				else if (key == QLatin1String("daily_profile")) { changed = assign(n->daily_profile, value.toBool()); }
				break;
			}
			case element_kind::road:
			{
				road_data *r = m.road(ref.id);
				if (!r) { break; }
				ch = property_change::change_network;
				if (key == QLatin1String("name")) { changed = assign(r->name, value.toString()); ch = property_change::change_none; }
				else if (key == QLatin1String("lanes_forward")) { changed = assign(r->lanes_forward, value.toInt()); }
				else if (key == QLatin1String("lanes_backward")) { changed = assign(r->lanes_backward, value.toInt()); }
				else if (key == QLatin1String("lane_width")) { changed = assign_d(r->lane_width, value.toDouble()); }
				else if (key == QLatin1String("speed_limit")) { changed = assign_d(r->speed_limit, value.toDouble()); ch = property_change::change_attributes; }
				else if (key == QLatin1String("overtaking")) { changed = assign(r->overtaking, value.toBool()); ch = property_change::change_attributes; }
				else if (key == QLatin1String("median")) { changed = assign(r->median, value.toBool()); }
				else if (key == QLatin1String("sidewalk_left")) { changed = assign(r->sidewalk_left, value.toBool()); }
				else if (key == QLatin1String("sidewalk_right")) { changed = assign(r->sidewalk_right, value.toBool()); }
				else if (key == QLatin1String("bike_lane")) { changed = assign(r->bike_lane, value.toBool()); }
				else if (key == QLatin1String("ditch")) { changed = assign(r->ditch, value.toBool()); }
				else if (key == QLatin1String("guard_rail")) { changed = assign(r->guard_rail, value.toBool()); ch = property_change::change_none; }
				else if (key == QLatin1String("level")) { changed = assign(r->level, std::max(-2, std::min(2, value.toInt() - 2))); }
				if (r->lanes_forward == 0 && r->lanes_backward == 0)
				{
					r->lanes_forward = 1; // a road needs at least one lane
				}
				break;
			}
			case element_kind::light:
			{
				light_data *l = m.light(ref.id);
				if (!l) { break; }
				if (key == QLatin1String("algorithm")) { changed = assign(l->algorithm, static_cast<light_algorithm>(value.toInt())); }
				else if (key == QLatin1String("offset")) { changed = assign_d(l->offset, value.toDouble()); }
				else if (key == QLatin1String("red_yellow")) { changed = assign_d(l->red_yellow, value.toDouble()); }
				else
				{
					static const QRegularExpression rx(QStringLiteral("^phase_(\\d+)_(\\w+)$"));
					const QRegularExpressionMatch mt = rx.match(key);
					if (mt.hasMatch())
					{
						const int k = mt.captured(1).toInt();
						const QString f = mt.captured(2);
						if (k >= 0 && k < static_cast<int>(l->phases.size()))
						{
							light_phase &ph = l->phases[k];
							const double v = value.toDouble();
							if (f == QLatin1String("green")) { changed = assign_d(ph.green, v); }
							else if (f == QLatin1String("yellow")) { changed = assign_d(ph.yellow, v); }
							else if (f == QLatin1String("all_red")) { changed = assign_d(ph.all_red, v); }
							else if (f == QLatin1String("min_green")) { changed = assign_d(ph.min_green, v); }
							else if (f == QLatin1String("max_green")) { changed = assign_d(ph.max_green, v); }
						}
					}
				}
				break;
			}
			case element_kind::sign:
			{
				sign_data *s = m.sign(ref.id);
				if (!s) { break; }
				ch = property_change::change_network;
				if (key == QLatin1String("key"))
				{
					const QList<sign_def> &all = sign_catalog::instance().all();
					const int i = value.toInt();
					if (i >= 0 && i < all.size())
					{
						changed = assign(s->key, all.at(i).key);
						if (changed)
						{
							s->value = all.at(i).default_value;
						}
					}
				}
				else if (key == QLatin1String("value")) { changed = assign(s->value, value.toInt()); }
				else if (key == QLatin1String("dir")) { changed = assign(s->dir, value.toInt() == 0 ? 1 : -1); }
				else if (key == QLatin1String("s")) { changed = assign_d(s->s, value.toDouble()); }
				break;
			}
			case element_kind::object:
			{
				object_data *o = m.object(ref.id);
				if (!o) { break; }
				ch = property_change::change_network;
				if (key == QLatin1String("s")) { changed = assign_d(o->s, value.toDouble()); }
				else if (key == QLatin1String("dir"))
				{
					const int v = (o->kind == object_kind::bus_stop) ? (value.toInt() == 0 ? 1 : -1) : dir3_value(value.toInt());
					changed = assign(o->dir, v);
				}
				else if (key == QLatin1String("lane")) { changed = assign(o->lane, value.toInt()); }
				else if (key == QLatin1String("length")) { changed = assign_d(o->length, value.toDouble()); }
				else if (key == QLatin1String("train_period")) { changed = assign_d(o->train_period, value.toDouble()); ch = property_change::change_none; }
				else if (key == QLatin1String("closed_time")) { changed = assign_d(o->closed_time, value.toDouble()); ch = property_change::change_none; }
				else if (key == QLatin1String("train_offset")) { changed = assign_d(o->train_offset, value.toDouble()); ch = property_change::change_none; }
				else if (key == QLatin1String("dwell_time")) { changed = assign_d(o->dwell_time, value.toDouble()); ch = property_change::change_none; }
				else if (key == QLatin1String("name")) { changed = assign(o->name, value.toString()); ch = property_change::change_none; }
				break;
			}
			default:
				break;
		}
		if (!changed)
		{
			return false;
		}
		if (ch == property_change::change_network)
		{
			m.touch_network();
		}
		else
		{
			m.touch();
		}
		if (change)
		{
			*change = ch;
		}
		return true;
	}
}
