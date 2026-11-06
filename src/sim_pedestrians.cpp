// Pedestrian part of the simulation (see simulation.h).
//
// A pedestrian walks from a dead-end node to another one. The route uses
// the roads with sidewalks. Along a road, the pedestrian walks on one
// sidewalk. In a junction, the pedestrian walks around the corners. To go to
// the other side of an arm, the pedestrian uses the crosswalk of that arm:
//   * with a traffic light: only if the vehicles of that arm have red and
//     the red time is long enough to cross (or in a pedestrian phase),
//   * without a traffic light (zebra): only if no vehicle comes near.
// A pedestrian on a crosswalk makes it "occupied": no vehicle enters it.

#include "simulation.h"
#include "road_network.h"
#include "world_model.h"
#include "logger.h"

#include <algorithm>
#include <cmath>

namespace
{
	const int max_pedestrians = 3000;

	polyline segment_line(const vec2 &a, const vec2 &b)
	{
		if (distance(a, b) < 0.05)
		{
			return polyline(std::vector<vec2>{ a, a + vec2(0.05, 0.0) });
		}
		return polyline(std::vector<vec2>{ a, b });
	}
}

void simulation::spawn_pedestrians(double dt)
{
	Q_UNUSED(dt);
	if (static_cast<int>(m_peds.size()) >= max_pedestrians)
	{
		return;
	}
	const double profile = demand_profile(clock());
	for (const auto &kv : m_model->nodes)
	{
		const node_data &n = kv.second;
		const net_junction *j = m_net->junction(n.id);
		if (!n.spawn || !j || j->arms.size() != 1)
		{
			continue;
		}
		const net_road *r = m_net->road(j->arms.front().road_id);
		if (!r || (!r->sidewalk_left && !r->sidewalk_right))
		{
			continue;
		}
		const double rate = n.pedestrians_per_hour * (n.daily_profile ? profile : 1.0) * m_ped_factor;
		if (rate < 0.01)
		{
			m_next_ped_spawn.erase(n.id);
			continue;
		}
		auto it = m_next_ped_spawn.find(n.id);
		if (it == m_next_ped_spawn.end())
		{
			m_next_ped_spawn[n.id] = m_time + random_exp(3600.0 / rate);
			continue;
		}
		if (m_time < it->second)
		{
			continue;
		}
		try_spawn_pedestrian(n.id);
		it->second = m_time + random_exp(3600.0 / rate);
	}
}

bool simulation::try_spawn_pedestrian(int node_id)
{
	std::vector<int> dests;
	for (const auto &kv : m_model->nodes)
	{
		const net_junction *j = m_net->junction(kv.first);
		if (kv.first == node_id || !kv.second.spawn || !j || j->arms.size() != 1)
		{
			continue;
		}
		const net_road *r = m_net->road(j->arms.front().road_id);
		if (r && (r->sidewalk_left || r->sidewalk_right))
		{
			dests.push_back(kv.first);
		}
	}
	if (dests.empty())
	{
		return false;
	}
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		const int dest = dests[m_rng() % dests.size()];
		const std::vector<std::pair<int, int>> route = m_routes.pedestrian_route(node_id, dest);
		if (route.empty())
		{
			continue;
		}
		sim_pedestrian p;
		p.id = m_next_id++;
		p.speed = 1.1 + 0.5 * random01();
		p.lateral = (random01() - 0.5) * 0.9;
		static const quint32 shirts[] = { 0xF7C6D9, 0xA8E6CF, 0xFFD3B6, 0xD5C6F2, 0xA7C7E7, 0xFDF1A6, 0xFFAAA5 };
		p.color = shirts[m_rng() % 7];
		p.origin_node = node_id;
		p.dest_node = dest;
		p.spawn_time = m_time;
		if (!build_pedestrian_legs(p, route))
		{
			continue;
		}
		m_peds.push_back(p);
		return true;
	}
	return false;
}

bool simulation::build_pedestrian_legs(sim_pedestrian &p, const std::vector<std::pair<int, int>> &route)
{
	p.legs.clear();
	// Sidewalk line for a road, a walking direction and a walking side
	// (ws +1: right side of the pedestrian, -1: left side).
	auto side_line = [this](int road_id, int wd, int ws) -> polyline
	{
		const net_road *r = m_net->road(road_id);
		if (!r)
		{
			return polyline();
		}
		const int road_side = -wd * ws; // +1 left of a -> b, -1 right
		const polyline &line = road_side > 0 ? r->sidewalk_left_line : r->sidewalk_right_line;
		if (line.empty())
		{
			return polyline();
		}
		return wd > 0 ? line : line.reversed();
	};
	auto add_leg = [&p](const polyline &path, int crosswalk)
	{
		if (path.empty())
		{
			return;
		}
		sim_pedestrian::leg l;
		l.path = path;
		l.crosswalk = crosswalk;
		p.legs.push_back(l);
	};

	int ws = side_line(route[0].first, route[0].second, 1).empty() ? -1 : 1;
	vec2 cursor;
	for (size_t k = 0; k < route.size(); ++k)
	{
		const int rid = route[k].first;
		const int wd = route[k].second;
		polyline line = side_line(rid, wd, ws);
		if (line.empty())
		{
			ws = -ws;
			line = side_line(rid, wd, ws);
			if (line.empty())
			{
				return false;
			}
		}
		if (k > 0)
		{
			add_leg(segment_line(cursor, line.front()), -1);
		}
		add_leg(line, -1);
		cursor = line.back();
		if (k + 1 == route.size())
		{
			break;
		}

		// The junction between this road and the next road
		const road_data *rd = m_model->road(rid);
		const int node = wd > 0 ? rd->node_b : rd->node_a;
		const net_junction *j = m_net->junction(node);
		if (!j || j->arms.size() < 2)
		{
			return false;
		}
		const int n = static_cast<int>(j->arms.size());
		int k_in = -1;
		int k_out = -1;
		for (int a = 0; a < n; ++a)
		{
			if (j->arms[a].road_id == rid && k_in < 0)
			{
				k_in = a;
			}
			if (j->arms[a].road_id == route[k + 1].first && a != k_in)
			{
				k_out = a;
			}
		}
		if (k_in < 0 || k_out < 0)
		{
			return false;
		}
		const int start_corner = ws > 0 ? k_in : (k_in - 1 + n) % n;
		int best_ws = 0;
		int best_dir = 0;
		int best_steps = 1000;
		for (int ws2 : { 1, -1 })
		{
			if (side_line(route[k + 1].first, route[k + 1].second, ws2).empty())
			{
				continue;
			}
			const int target = ws2 > 0 ? (k_out - 1 + n) % n : k_out;
			for (int dir : { 1, -1 })
			{
				const int steps = dir > 0 ? (target - start_corner + n) % n : (start_corner - target + n) % n;
				bool ok = true;
				for (int i = 0; i < steps && ok; ++i)
				{
					const int arm = dir > 0 ? (start_corner + 1 + i) % n : (start_corner - i + n) % n;
					ok = j->arms[arm].crosswalk >= 0;
				}
				if (ok && (steps < best_steps || (steps == best_steps && ws2 > best_ws)))
				{
					best_steps = steps;
					best_ws = ws2;
					best_dir = dir;
				}
			}
		}
		if (best_ws == 0)
		{
			return false; // no path with crosswalks: the pedestrian does not cross here
		}
		int corner = start_corner;
		add_leg(segment_line(cursor, j->corners[corner]), -1);
		cursor = j->corners[corner];
		for (int i = 0; i < best_steps; ++i)
		{
			const int arm = best_dir > 0 ? (corner + 1) % n : corner;
			const net_crosswalk &cw = m_net->crosswalks[j->arms[arm].crosswalk];
			const vec2 entry = best_dir > 0 ? cw.a : cw.b;
			const vec2 exit = best_dir > 0 ? cw.b : cw.a;
			add_leg(segment_line(cursor, entry), -1);
			add_leg(segment_line(entry, exit), cw.id);
			corner = best_dir > 0 ? (corner + 1) % n : (corner - 1 + n) % n;
			add_leg(segment_line(exit, j->corners[corner]), -1);
			cursor = j->corners[corner];
		}
		ws = best_ws;
	}
	return !p.legs.empty();
}

bool simulation::crossing_allowed(int crosswalk_id, double speed) const
{
	const net_crosswalk &cw = m_net->crosswalks[crosswalk_id];
	const double cross_time = cw.length / std::max(0.5, speed) + 1.5;
	const light_runtime *lr = light_at_node(cw.node_id);
	if (lr)
	{
		const light_data *ld = m_model->light(lr->light_id);
		if (ld && lr->phase < static_cast<int>(ld->phases.size()) && ld->phases[lr->phase].pedestrian_only)
		{
			if (lr->stage == 1)
			{
				return lr->stage_left > cross_time * 0.6;
			}
			return false;
		}
		const signal_color col = signal_for(cw.node_id, cw.road_id);
		if (col != signal_color::red)
		{
			return false;
		}
		const double remaining = signal_remaining(cw.node_id, cw.road_id);
		if (remaining < cross_time)
		{
			return false;
		}
		// Walk interval (like a pedestrian signal): the pedestrians start only
		// in the first part of the red time of this arm. Later, the turning
		// vehicles can use the crosswalk area.
		if (ld)
		{
			double cycle = 0.0;
			double green = 0.0;
			for (const light_phase &ph : ld->phases)
			{
				cycle += ph.green + ph.yellow + ph.all_red + ld->red_yellow;
				if (!ph.pedestrian_only && std::find(ph.green_roads.begin(), ph.green_roads.end(), cw.road_id) != ph.green_roads.end())
				{
					green += ph.green + ph.yellow;
				}
			}
			const double red_total = cycle - green;
			const double walk = std::max(8.0, std::min(15.0, red_total * 0.4));
			if (red_total - remaining > walk)
			{
				return false;
			}
		}
	}
	else
	{
		// Zebra without lights: wait for a safe gap in the traffic.
		for (int lid : cw.lanes_in)
		{
			const net_lane &l = m_net->lanes[lid];
			for (int vi : m_on_lane[lid])
			{
				const sim_vehicle &w = m_vehicles[vi];
				const double d = (l.length() - 3.5) - w.s;   // front of the vehicle to the crosswalk
				if (d < -6.0)
				{
					continue;
				}
				if (d < 2.0 || w.committed)
				{
					return false;
				}
				if (w.v > 0.5 && d / w.v < 3.5)
				{
					return false;
				}
				break; // only the nearest vehicle of the lane
			}
		}
	}
	// Vehicles that turn into this road pass the crosswalk soon: let them go first.
	const net_junction *j = m_net->junction(cw.node_id);
	if (j)
	{
		for (int cid : j->connectors)
		{
			const net_connector &c = m_net->connectors[cid];
			if (std::find(cw.lanes_out.begin(), cw.lanes_out.end(), c.to_lane) == cw.lanes_out.end())
			{
				continue;
			}
			if (!m_on_conn[cid].empty() || m_reservations[cid] > 0)
			{
				return false;
			}
		}
	}
	for (int lid : cw.lanes_out)
	{
		const int last = last_vehicle_on_lane(lid);
		if (last >= 0)
		{
			const sim_vehicle &w = m_vehicles[last];
			if (w.s - w.p->length < m_net->lanes[lid].start_cw_s1 + 0.5)
			{
				return false;
			}
		}
	}
	return true;
}

void simulation::update_pedestrians(double dt)
{
	std::fill(m_cw_users.begin(), m_cw_users.end(), 0);
	for (sim_pedestrian &p : m_peds)
	{
		if (p.leg_index >= static_cast<int>(p.legs.size()))
		{
			continue;
		}
		const sim_pedestrian::leg *leg = &p.legs[p.leg_index];
		bool counted = false;
		if (leg->crosswalk >= 0 && !p.on_crosswalk)
		{
			if (crossing_allowed(leg->crosswalk, p.speed))
			{
				p.on_crosswalk = true;
				p.waiting = false;
				m_cw_users[leg->crosswalk]++;   // other pedestrians and vehicles see it now
				counted = true;
			}
			else
			{
				p.waiting = true;
				p.wait_time += dt;
			}
		}
		if (!p.waiting)
		{
			p.s += p.speed * dt;
			while (p.s >= leg->path.length())
			{
				p.s -= leg->path.length();
				p.on_crosswalk = false;
				++p.leg_index;
				if (p.leg_index >= static_cast<int>(p.legs.size()))
				{
					break;
				}
				leg = &p.legs[p.leg_index];
				if (leg->crosswalk >= 0)
				{
					p.s = 0.0; // stop at the curb and look
					break;
				}
			}
		}
		if (p.leg_index >= static_cast<int>(p.legs.size()))
		{
			continue;
		}
		if (p.on_crosswalk && leg->crosswalk >= 0 && !counted)
		{
			m_cw_users[leg->crosswalk]++;
		}
		vec2 pt;
		vec2 t;
		leg->path.sample(p.s, &pt, &t);
		const double lat = leg->crosswalk >= 0 ? p.lateral * 1.2 : p.lateral;
		p.pos = pt + t.left() * lat;
		p.dir = t;
	}
	m_peds.erase(std::remove_if(m_peds.begin(), m_peds.end(),
		[](const sim_pedestrian &p) { return p.leg_index >= static_cast<int>(p.legs.size()); }), m_peds.end());
}
