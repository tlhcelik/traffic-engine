#include "world_model.h"

#include <algorithm>
#include <set>

world_model::world_model()
	: m_next_id(1)
{
}

void world_model::clear()
{
	nodes.clear();
	roads.clear();
	lights.clear();
	signs.clear();
	objects.clear();
	ground = ground_material::grass;
	name.clear();
	description.clear();
	start_clock = 8.0 * 3600.0;
	geo_lat = 0.0;
	geo_lon = 0.0;
	m_next_id = 1;
	touch_network();
}

int world_model::next_id()
{
	return m_next_id++;
}

void world_model::reserve_id(int id)
{
	if (id >= m_next_id)
	{
		m_next_id = id + 1;
	}
}

// ---------------------------------------------------------------------------
// access
// ---------------------------------------------------------------------------
namespace
{
	template <typename T>
	T *find_in(std::map<int, T> &m, int id)
	{
		auto it = m.find(id);
		return it == m.end() ? nullptr : &it->second;
	}

	template <typename T>
	const T *find_in(const std::map<int, T> &m, int id)
	{
		auto it = m.find(id);
		return it == m.end() ? nullptr : &it->second;
	}
}

node_data *world_model::node(int id) { return find_in(nodes, id); }
const node_data *world_model::node(int id) const { return find_in(nodes, id); }
road_data *world_model::road(int id) { return find_in(roads, id); }
const road_data *world_model::road(int id) const { return find_in(roads, id); }
light_data *world_model::light(int id) { return find_in(lights, id); }
const light_data *world_model::light(int id) const { return find_in(lights, id); }
sign_data *world_model::sign(int id) { return find_in(signs, id); }
const sign_data *world_model::sign(int id) const { return find_in(signs, id); }
object_data *world_model::object(int id) { return find_in(objects, id); }
const object_data *world_model::object(int id) const { return find_in(objects, id); }

std::vector<int> world_model::roads_at_node(int node_id) const
{
	std::vector<int> out;
	for (const auto &kv : roads)
	{
		if (kv.second.node_a == node_id || kv.second.node_b == node_id)
		{
			out.push_back(kv.first);
		}
	}
	return out;
}

int world_model::light_at_node(int node_id) const
{
	for (const auto &kv : lights)
	{
		if (kv.second.node_id == node_id)
		{
			return kv.first;
		}
	}
	return -1;
}

polyline world_model::road_centerline(const road_data &r) const
{
	const node_data *a = node(r.node_a);
	const node_data *b = node(r.node_b);
	if (!a || !b)
	{
		return polyline();
	}
	std::vector<vec2> ctrl;
	ctrl.reserve(r.shape.size() + 2);
	ctrl.push_back(a->pos);
	ctrl.insert(ctrl.end(), r.shape.begin(), r.shape.end());
	ctrl.push_back(b->pos);
	return polyline(geom::smooth_curve(ctrl, 1.0));
}

double world_model::total_road_km() const
{
	double total = 0.0;
	for (const auto &kv : roads)
	{
		total += road_centerline(kv.second).length();
	}
	return total / 1000.0;
}

// ---------------------------------------------------------------------------
// edit operations
// ---------------------------------------------------------------------------
int world_model::add_node(const vec2 &pos)
{
	node_data n;
	n.id = next_id();
	n.pos = pos;
	nodes[n.id] = n;
	touch_network();
	return n.id;
}

int world_model::add_road(int node_a, int node_b, const std::vector<vec2> &shape, const road_data &templ)
{
	if (node_a == node_b || !node(node_a) || !node(node_b))
	{
		return -1;
	}
	road_data r = templ;
	r.id = next_id();
	r.node_a = node_a;
	r.node_b = node_b;
	r.shape = shape;
	roads[r.id] = r;
	touch_network();
	return r.id;
}

int world_model::split_road(int road_id, double s)
{
	road_data *r = road(road_id);
	if (!r)
	{
		return -1;
	}
	const polyline center = road_centerline(*r);
	const double len = center.length();
	if (s < 1.0 || s > len - 1.0)
	{
		return -1;
	}
	const int n = add_node(center.point_at(s));

	// Control points before and after the split point
	std::vector<vec2> before;
	std::vector<vec2> after;
	for (const vec2 &p : r->shape)
	{
		if (center.project(p) < s)
		{
			before.push_back(p);
		}
		else
		{
			after.push_back(p);
		}
	}

	road_data second = *r;
	second.id = next_id();
	second.node_a = n;
	second.shape = after;
	const int old_b = r->node_b;
	r->node_b = n;
	r->shape = before;
	roads[second.id] = second;

	// Signs and objects after the split point go to the second road.
	for (auto &kv : signs)
	{
		if (kv.second.road_id == road_id && kv.second.s > s)
		{
			kv.second.road_id = second.id;
			kv.second.s -= s;
		}
	}
	for (auto &kv : objects)
	{
		if (kv.second.road_id == road_id && kv.second.s > s)
		{
			kv.second.road_id = second.id;
			kv.second.s -= s;
		}
	}
	// At the old end node, the incoming road is now the second road.
	for (auto &kv : lights)
	{
		if (kv.second.node_id != old_b)
		{
			continue;
		}
		for (light_phase &ph : kv.second.phases)
		{
			std::replace(ph.green_roads.begin(), ph.green_roads.end(), road_id, second.id);
		}
	}
	touch_network();
	return n;
}

void world_model::remove(const element_ref &ref)
{
	switch (ref.kind)
	{
		case element_kind::node:
		{
			for (int rid : roads_at_node(ref.id))
			{
				remove(element_ref(element_kind::road, rid));
			}
			const int lid = light_at_node(ref.id);
			if (lid >= 0)
			{
				lights.erase(lid);
			}
			nodes.erase(ref.id);
			break;
		}
		case element_kind::road:
		{
			const road_data *r = road(ref.id);
			if (!r)
			{
				return;
			}
			const int a = r->node_a;
			const int b = r->node_b;
			for (auto it = signs.begin(); it != signs.end();)
			{
				it = (it->second.road_id == ref.id) ? signs.erase(it) : std::next(it);
			}
			for (auto it = objects.begin(); it != objects.end();)
			{
				it = (it->second.road_id == ref.id) ? objects.erase(it) : std::next(it);
			}
			roads.erase(ref.id);
			// A node without roads has no use: remove it.
			for (int nid : { a, b })
			{
				if (roads_at_node(nid).empty())
				{
					const int lid = light_at_node(nid);
					if (lid >= 0)
					{
						lights.erase(lid);
					}
					nodes.erase(nid);
				}
			}
			repair_lights();
			break;
		}
		case element_kind::light:
			lights.erase(ref.id);
			break;
		case element_kind::sign:
			signs.erase(ref.id);
			break;
		case element_kind::object:
			objects.erase(ref.id);
			break;
		default:
			return;
	}
	touch_network();
}

void world_model::move_node(int node_id, const vec2 &pos)
{
	node_data *n = node(node_id);
	if (!n)
	{
		return;
	}
	n->pos = pos;
	touch_network();
}

int world_model::add_light(int node_id)
{
	const int existing = light_at_node(node_id);
	if (existing >= 0)
	{
		return existing;
	}
	if (!node(node_id) || roads_at_node(node_id).size() < 2)
	{
		return -1;
	}
	light_data l;
	l.id = next_id();
	l.node_id = node_id;
	default_light_program(l);
	lights[l.id] = l;
	touch_network();
	return l.id;
}

void world_model::default_light_program(light_data &light) const
{
	light.phases.clear();
	const node_data *n = node(light.node_id);
	if (!n)
	{
		return;
	}
	// Incoming roads (with lanes to the node) and their direction from the node
	struct arm
	{
		int road_id;
		double angle;
	};
	std::vector<arm> arms;
	for (int rid : roads_at_node(light.node_id))
	{
		const road_data &r = roads.at(rid);
		const bool at_b = (r.node_b == light.node_id);
		const int incoming_lanes = at_b ? r.lanes_forward : r.lanes_backward;
		if (incoming_lanes <= 0)
		{
			continue;
		}
		const polyline c = road_centerline(r);
		const vec2 out_dir = at_b ? -c.tangent_at(c.length()) : c.tangent_at(0.0);
		arms.push_back({ rid, out_dir.angle() });
	}
	// Pair the opposite roads (angle difference more than 150 degrees)
	std::vector<bool> used(arms.size(), false);
	for (size_t i = 0; i < arms.size(); ++i)
	{
		if (used[i])
		{
			continue;
		}
		used[i] = true;
		light_phase ph;
		ph.green_roads.push_back(arms[i].road_id);
		int best = -1;
		double best_diff = 0.0;
		for (size_t j = i + 1; j < arms.size(); ++j)
		{
			if (used[j])
			{
				continue;
			}
			const double diff = std::abs(wrap_angle(arms[i].angle - arms[j].angle));
			if (diff > 150.0 * M_PI / 180.0 && diff > best_diff)
			{
				best = static_cast<int>(j);
				best_diff = diff;
			}
		}
		if (best >= 0)
		{
			used[best] = true;
			ph.green_roads.push_back(arms[best].road_id);
		}
		light.phases.push_back(ph);
	}
}

void world_model::repair_lights()
{
	for (auto it = lights.begin(); it != lights.end();)
	{
		light_data &l = it->second;
		if (!node(l.node_id))
		{
			it = lights.erase(it);
			continue;
		}
		const std::vector<int> at = roads_at_node(l.node_id);
		const std::set<int> valid(at.begin(), at.end());
		for (light_phase &ph : l.phases)
		{
			ph.green_roads.erase(std::remove_if(ph.green_roads.begin(), ph.green_roads.end(),
				[&valid](int rid) { return valid.count(rid) == 0; }), ph.green_roads.end());
		}
		l.phases.erase(std::remove_if(l.phases.begin(), l.phases.end(),
			[](const light_phase &ph) { return ph.green_roads.empty() && !ph.pedestrian_only; }), l.phases.end());
		if (l.phases.empty())
		{
			default_light_program(l);
		}
		++it;
	}
}

// ---------------------------------------------------------------------------
// names
// ---------------------------------------------------------------------------
QString ground_name(ground_material g)
{
	switch (g)
	{
		case ground_material::grass: return QStringLiteral("grass");
		case ground_material::soil: return QStringLiteral("soil");
		case ground_material::sand: return QStringLiteral("sand");
		case ground_material::snow: return QStringLiteral("snow");
		case ground_material::concrete: return QStringLiteral("concrete");
		case ground_material::asphalt: return QStringLiteral("asphalt");
	}
	return QStringLiteral("grass");
}

ground_material ground_from_name(const QString &name)
{
	const QString n = name.trimmed().toLower();
	if (n == QLatin1String("soil")) { return ground_material::soil; }
	if (n == QLatin1String("sand")) { return ground_material::sand; }
	if (n == QLatin1String("snow")) { return ground_material::snow; }
	if (n == QLatin1String("concrete")) { return ground_material::concrete; }
	if (n == QLatin1String("asphalt")) { return ground_material::asphalt; }
	return ground_material::grass;
}

QString object_kind_name(object_kind k)
{
	switch (k)
	{
		case object_kind::cone: return QStringLiteral("cone");
		case object_kind::barrier: return QStringLiteral("barrier");
		case object_kind::road_closure: return QStringLiteral("road_closure");
		case object_kind::level_crossing: return QStringLiteral("level_crossing");
		case object_kind::bus_stop: return QStringLiteral("bus_stop");
	}
	return QStringLiteral("cone");
}

object_kind object_kind_from_name(const QString &name)
{
	const QString n = name.trimmed().toLower();
	if (n == QLatin1String("barrier")) { return object_kind::barrier; }
	if (n == QLatin1String("road_closure")) { return object_kind::road_closure; }
	if (n == QLatin1String("level_crossing")) { return object_kind::level_crossing; }
	if (n == QLatin1String("bus_stop")) { return object_kind::bus_stop; }
	return object_kind::cone;
}

QString light_algorithm_name(light_algorithm a)
{
	switch (a)
	{
		case light_algorithm::fixed_time: return QStringLiteral("fixed_time");
		case light_algorithm::actuated: return QStringLiteral("actuated");
		case light_algorithm::external: return QStringLiteral("external");
	}
	return QStringLiteral("fixed_time");
}

light_algorithm light_algorithm_from_name(const QString &name)
{
	const QString n = name.trimmed().toLower();
	if (n == QLatin1String("actuated")) { return light_algorithm::actuated; }
	if (n == QLatin1String("external")) { return light_algorithm::external; }
	return light_algorithm::fixed_time;
}
