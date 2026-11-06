#include "world_layout.h"
#include "road_network.h"
#include "workspace.h"

#include <algorithm>
#include <cmath>

namespace world_layout
{
	std::vector<signal_head> signal_heads(const world_model &m, const road_network &net)
	{
		std::vector<signal_head> out;
		for (const auto &kv : m.lights)
		{
			const net_junction *j = net.junction(kv.second.node_id);
			if (!j)
			{
				continue;
			}
			for (const net_arm &arm : j->arms)
			{
				const std::vector<int> &ls = net.lanes_of(arm.road_id, arm.at_a ? -1 : 1);
				if (ls.empty())
				{
					continue;
				}
				const net_lane &l = net.lanes[ls.front()];
				const net_road *r = net.road(arm.road_id);
				vec2 p;
				vec2 t;
				l.path.sample(l.stop_line, &p, &t);
				signal_head h;
				h.light_id = kv.first;
				h.node_id = kv.second.node_id;
				h.road_id = arm.road_id;
				h.dir = t;
				h.pos = p + t.right() * (l.width / 2.0 + (r ? r->bike_width : 0.0) + 1.1) + t * 0.6;
				out.push_back(h);
			}
		}
		return out;
	}

	bool sign_pose(const world_model &m, const road_network &net, const sign_data &s, vec2 *pos, vec2 *dir)
	{
		Q_UNUSED(m);
		const net_road *r = net.road(s.road_id);
		if (!r)
		{
			return false;
		}
		vec2 p;
		vec2 t;
		r->center.sample(std::max(0.0, std::min(r->center.length(), s.s)), &p, &t);
		double lateral = 0.0;
		if (s.dir > 0)
		{
			lateral = r->edge_right - r->bike_width - (r->sidewalk_right ? r->sidewalk_width * 0.8 : 0.9);
		}
		else
		{
			lateral = r->edge_left + r->bike_width + (r->sidewalk_left ? r->sidewalk_width * 0.8 : 0.9);
		}
		*pos = p + t.left() * lateral;
		*dir = s.dir > 0 ? t : -t;
		return true;
	}

	bool object_pose(const world_model &m, const road_network &net, const object_data &o, vec2 *pos, vec2 *dir, double *half_length)
	{
		Q_UNUSED(m);
		const net_road *r = net.road(o.road_id);
		if (!r)
		{
			return false;
		}
		vec2 p;
		vec2 t;
		r->center.sample(std::max(0.0, std::min(r->center.length(), o.s)), &p, &t);
		*dir = t;
		*half_length = 1.5;
		switch (o.kind)
		{
			case object_kind::cone:
			case object_kind::barrier:
			{
				const int d = o.dir == 0 ? 1 : o.dir;
				const std::vector<int> &ls = net.lanes_of(o.road_id, d);
				if (!ls.empty())
				{
					const int idx = o.lane < 0 ? 0 : std::min<int>(o.lane, static_cast<int>(ls.size()) - 1);
					const net_lane &l = net.lanes[ls[idx]];
					const double s_lane = net.lane_s_from_road_s(l, o.s);
					l.path.sample(s_lane, &p, &t);
					*dir = t;
				}
				*pos = p;
				*half_length = o.length / 2.0;
				return true;
			}
			case object_kind::bus_stop:
			{
				const double lateral = o.dir > 0
					? r->edge_right - r->bike_width - (r->sidewalk_right ? r->sidewalk_width * 0.5 : 1.2)
					: r->edge_left + r->bike_width + (r->sidewalk_left ? r->sidewalk_width * 0.5 : 1.2);
				*pos = p + t.left() * lateral;
				*dir = o.dir > 0 ? t : -t;
				*half_length = 5.0;
				return true;
			}
			default:
				*pos = p;
				*half_length = std::max(std::abs(r->edge_left), std::abs(r->edge_right));
				return true;
		}
	}

	int node_at(const world_model &m, const vec2 &p, double tolerance)
	{
		int best = -1;
		double best_d = tolerance;
		for (const auto &kv : m.nodes)
		{
			const double d = distance(kv.second.pos, p);
			if (d <= best_d)
			{
				best_d = d;
				best = kv.first;
			}
		}
		return best;
	}

	element_ref pick(const workspace &ws, const vec2 &p, double tolerance, bool include_dynamic, bool prefer_nodes)
	{
		const world_model &m = ws.model();
		const road_network &net = ws.network();
		element_ref best;
		double best_d = tolerance;
		auto consider = [&](element_kind k, int id, double d)
		{
			if (d <= best_d)
			{
				best_d = d;
				best = element_ref(k, id);
			}
		};

		if (include_dynamic)
		{
			for (const sim_vehicle &v : ws.sim().vehicles())
			{
				const vec2 rel = p - v.pos;
				const double lx = std::abs(dot(rel, v.dir)) - v.p->length / 2.0;
				const double ly = std::abs(cross(v.dir, rel)) - v.p->width / 2.0;
				consider(element_kind::vehicle, v.id, std::max(0.0, std::max(lx, ly)) * 0.8);
			}
			for (const sim_pedestrian &pd : ws.sim().pedestrians())
			{
				consider(element_kind::pedestrian, pd.id, std::max(0.0, distance(p, pd.pos) - 0.4));
			}
		}
		for (const signal_head &h : signal_heads(m, net))
		{
			consider(element_kind::light, h.light_id, std::max(0.0, distance(p, h.pos) - 0.8));
		}
		for (const auto &kv : m.signs)
		{
			vec2 pos;
			vec2 dir;
			if (sign_pose(m, net, kv.second, &pos, &dir))
			{
				consider(element_kind::sign, kv.first, std::max(0.0, distance(p, pos) - 0.8));
			}
		}
		for (const auto &kv : m.objects)
		{
			vec2 pos;
			vec2 dir;
			double hl = 1.0;
			if (object_pose(m, net, kv.second, &pos, &dir, &hl))
			{
				const vec2 rel = p - pos;
				const double lx = std::abs(dot(rel, dir)) - hl;
				const double ly = std::abs(cross(dir, rel)) - 1.5;
				consider(element_kind::object, kv.first, std::max(0.0, std::max(lx, ly)));
			}
		}
		for (const auto &kv : m.nodes)
		{
			const double d = distance(p, kv.second.pos);
			consider(element_kind::node, kv.first, std::max(0.0, d - (prefer_nodes ? 2.5 : 0.8)));
		}
		if (!best.valid())
		{
			// A junction with a light: a click in the junction selects the light.
			for (const auto &kv : m.lights)
			{
				const net_junction *j = net.junction(kv.second.node_id);
				if (j && j->polygon.size() >= 3 && geom::point_in_polygon(p, j->polygon))
				{
					return element_ref(element_kind::light, kv.first);
				}
			}
			double s = 0.0;
			const int rid = net.road_at(p, tolerance, &s, nullptr);
			if (rid >= 0)
			{
				return element_ref(element_kind::road, rid);
			}
		}
		return best;
	}
}
