#include "road_network.h"
#include "sign_catalog.h"
#include "logger.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

namespace
{
	typedef bg::model::point<double, 2, bg::cs::cartesian> bpoint;
	typedef bg::model::box<bpoint> bbox;
	typedef std::pair<bbox, int> rvalue;

	const double sidewalk_w = 2.0;
	const double bike_w = 1.5;
	const double ditch_w = 1.5;
	const double median_w = 2.0;
	const double crosswalk_gap = 0.5;    // from the junction edge to the crosswalk
	const double crosswalk_w = 3.0;      // width of the zebra
	const double stop_gap = 1.0;         // stop line before the crosswalk

	const double deg = M_PI / 180.0;
}

struct road_network::index_impl
{
	bgi::rtree<rvalue, bgi::quadratic<16>> tree;
};

// ---------------------------------------------------------------------------
// net_lane
// ---------------------------------------------------------------------------
double net_lane::limit_at(double s) const
{
	double v = base_limit;
	for (const net_zone &z : limits)
	{
		if (z.s <= s)
		{
			v = z.value;
		}
		else
		{
			break;
		}
	}
	return v;
}

bool net_lane::overtaking_at(double s) const
{
	for (const auto &z : no_overtaking)
	{
		if (s >= z.first && s < z.second)
		{
			return false;
		}
	}
	return true;
}

bool net_lane::blocked_at(double s0, double s1) const
{
	for (const net_block &b : blocks)
	{
		if (b.s1 >= s0 && b.s0 <= s1)
		{
			return true;
		}
	}
	return false;
}

QString turn_name(turn_kind t)
{
	switch (t)
	{
		case turn_kind::right: return QStringLiteral("right");
		case turn_kind::straight: return QStringLiteral("straight");
		case turn_kind::left: return QStringLiteral("left");
		case turn_kind::uturn: return QStringLiteral("U-turn");
	}
	return QStringLiteral("straight");
}

// ---------------------------------------------------------------------------
// road_network
// ---------------------------------------------------------------------------
road_network::road_network()
	: m_index(new index_impl)
	, m_built_revision(0)
{
}

road_network::~road_network() = default;

const net_road *road_network::road(int id) const
{
	const auto it = roads.find(id);
	return it == roads.end() ? nullptr : &it->second;
}

const net_junction *road_network::junction(int node_id) const
{
	const auto it = junctions.find(node_id);
	return it == junctions.end() ? nullptr : &it->second;
}

const std::vector<int> &road_network::lanes_of(int road_id, int dir) const
{
	const auto it = m_lanes_by_dir.find(dir_key(road_id, dir));
	return it == m_lanes_by_dir.end() ? m_empty : it->second;
}

double road_network::lane_s_from_road_s(const net_lane &lane, double road_s) const
{
	const net_road *r = road(lane.road_id);
	if (!r)
	{
		return 0.0;
	}
	const double tl = std::max(0.01, r->trimmed.length());
	const double t = lane.dir > 0 ? (road_s - r->trim_a) : (r->center.length() - road_s - r->trim_b);
	return t * lane.length() / tl;
}

double road_network::road_s_from_lane_s(const net_lane &lane, double lane_s) const
{
	const net_road *r = road(lane.road_id);
	if (!r)
	{
		return 0.0;
	}
	const double t = lane_s * std::max(0.01, r->trimmed.length()) / std::max(0.01, lane.length());
	return lane.dir > 0 ? r->trim_a + t : r->center.length() - r->trim_b - t;
}

void road_network::build(const world_model &model)
{
	QElapsedTimer timer;
	timer.start();

	lanes.clear();
	connectors.clear();
	crosswalks.clear();
	roads.clear();
	junctions.clear();
	m_lanes_by_dir.clear();
	total_lane_km = 0.0;

	build_roads(model);
	build_junctions(model);
	build_levels(model);
	build_lanes(model);
	build_connectors();
	build_conflicts();
	build_crosswalks();
	build_turn_clearance();
	apply_signs(model);
	apply_objects(model);
	build_index();

	for (const net_lane &l : lanes)
	{
		total_lane_km += l.length() / 1000.0;
	}
	m_built_revision = model.network_revision;
	TE_DEBUG("NETWORK", QStringLiteral("Road network is ready: %1 roads, %2 junctions, %3 lanes, %4 connectors, %5 crosswalks (%6 ms)")
		.arg(roads.size()).arg(junctions.size()).arg(lanes.size()).arg(connectors.size()).arg(crosswalks.size())
		.arg(timer.elapsed()));
}

void road_network::refresh_attributes(const world_model &model)
{
	for (net_lane &l : lanes)
	{
		const road_data *rd = model.road(l.road_id);
		if (!rd)
		{
			continue;
		}
		l.base_limit = std::max(5.0, rd->speed_limit) / 3.6;
		l.limits.clear();
		l.no_overtaking.clear();
		if (!rd->overtaking)
		{
			l.no_overtaking.push_back(std::make_pair(0.0, l.length() + 1.0));
		}
	}
	for (net_connector &c : connectors)
	{
		const double radius = c.path.min_radius();
		c.max_speed = std::min(std::min(lanes[c.from_lane].base_limit, lanes[c.to_lane].base_limit),
			std::max(3.0, std::sqrt(2.2 * radius)));
	}
	apply_signs(model);
}

void road_network::build_roads(const world_model &m)
{
	for (const auto &kv : m.roads)
	{
		const road_data &r = kv.second;
		net_road nr;
		nr.road_id = r.id;
		nr.center = m.road_centerline(r);
		if (nr.center.empty() || nr.center.length() < 1.0)
		{
			continue;
		}
		const double lw = std::max(2.0, r.lane_width);
		const int lf = std::max(0, r.lanes_forward);
		const int lb = std::max(0, r.lanes_backward);
		const bool two_way = lf > 0 && lb > 0;
		nr.median_width = (r.median && two_way) ? median_w : 0.0;
		if (two_way)
		{
			nr.edge_right = -(nr.median_width / 2.0 + lf * lw);
			nr.edge_left = nr.median_width / 2.0 + lb * lw;
		}
		else
		{
			const double w = (lf + lb) * lw;
			nr.edge_right = -w / 2.0;
			nr.edge_left = w / 2.0;
		}
		nr.bike_width = r.bike_lane ? bike_w : 0.0;
		nr.sidewalk_width = sidewalk_w;
		nr.ditch_width = r.ditch ? ditch_w : 0.0;
		nr.sidewalk_left = r.sidewalk_left;
		nr.sidewalk_right = r.sidewalk_right;
		nr.outer_left = nr.edge_left + nr.bike_width + (r.sidewalk_left ? sidewalk_w : 0.0) + nr.ditch_width;
		nr.outer_right = nr.edge_right - nr.bike_width - (r.sidewalk_right ? sidewalk_w : 0.0) - nr.ditch_width;
		roads[r.id] = nr;
	}
}

void road_network::build_junctions(const world_model &m)
{
	// Roads and lights of each node (one pass; large imported networks)
	std::map<int, std::vector<int>> roads_by_node;
	for (const auto &kv : m.roads)
	{
		roads_by_node[kv.second.node_a].push_back(kv.first);
		roads_by_node[kv.second.node_b].push_back(kv.first);
	}
	std::map<int, int> light_by_node;
	for (const auto &kv : m.lights)
	{
		light_by_node.insert(std::make_pair(kv.second.node_id, kv.first));
	}
	static const std::vector<int> no_roads;
	for (const auto &kv : m.nodes)
	{
		const node_data &n = kv.second;
		net_junction j;
		j.node_id = n.id;
		j.pos = n.pos;
		const auto lit = light_by_node.find(n.id);
		j.light_id = lit != light_by_node.end() ? lit->second : -1;
		const auto rit_node = roads_by_node.find(n.id);
		for (int rid : (rit_node != roads_by_node.end() ? rit_node->second : no_roads))
		{
			const auto rit = roads.find(rid);
			if (rit == roads.end())
			{
				continue;
			}
			const road_data *rd = m.road(rid);
			net_arm a;
			a.road_id = rid;
			a.at_a = (rd->node_a == n.id);
			const polyline &c = rit->second.center;
			a.dir = a.at_a ? c.tangent_at(0.0) : -c.tangent_at(c.length());
			a.angle = a.dir.angle();
			j.arms.push_back(a);
		}
		if (j.arms.empty())
		{
			continue;
		}
		std::sort(j.arms.begin(), j.arms.end(), [](const net_arm &x, const net_arm &y) { return x.angle < y.angle; });

		if (j.arms.size() >= 2)
		{
			double max_hw = 0.0;
			for (const net_arm &a : j.arms)
			{
				const net_road &r = roads[a.road_id];
				max_hw = std::max(max_hw, std::max(std::abs(r.edge_left), std::abs(r.edge_right)) + r.bike_width);
			}
			// Smallest angle between two neighbor arms
			double min_gap = 2.0 * M_PI;
			for (size_t i = 0; i < j.arms.size(); ++i)
			{
				const double a0 = j.arms[i].angle;
				const double a1 = j.arms[(i + 1) % j.arms.size()].angle + (i + 1 == j.arms.size() ? 2.0 * M_PI : 0.0);
				min_gap = std::min(min_gap, a1 - a0);
			}
			if (j.arms.size() == 2 && min_gap > 150.0 * deg && j.light_id < 0)
			{
				// Nearly straight continuation: only a small seam
				j.radius = 1.0;
			}
			else
			{
				j.radius = max_hw + 2.0;
				if (min_gap < 70.0 * deg)
				{
					j.radius *= 1.0 + (70.0 * deg - min_gap) / (70.0 * deg);
				}
			}
			// Do not trim more than 40 % of the shortest arm
			for (const net_arm &a : j.arms)
			{
				j.radius = std::min(j.radius, roads[a.road_id].center.length() * 0.4);
			}
			// Trim of each arm: after the trim, the carriageway of the arm must
			// not touch the carriageway of another arm (sharp angles, slip roads).
			// Else the vehicles that wait at the stop line of one arm overlap the
			// vehicles that start on the other arm.
			auto half_width = [](const net_road &r) { return std::max(std::abs(r.edge_left), std::abs(r.edge_right)) + r.bike_width; };
			for (net_arm &a : j.arms)
			{
				a.trim = j.radius;
				double wanted = j.radius; // the trim without the limit of the road length
				const net_road &ra = roads[a.road_id];
				const polyline out_a = a.at_a ? ra.center : ra.center.reversed();
				const double s_max = out_a.length() * 0.45;
				for (const net_arm &b : j.arms)
				{
					if (&b == &a)
					{
						continue;
					}
					const net_road &rb = roads[b.road_id];
					const polyline out_b = b.at_a ? rb.center : rb.center.reversed();
					const double need = half_width(ra) + half_width(rb) + 0.4;
					double last_bad = -1.0;
					for (double s = 0.0; s <= s_max; s += 0.5)
					{
						const vec2 p = out_a.point_at(s);
						double d = 0.0;
						const double t = out_b.project(p, &d);
						if (t < 0.01 && dot(p - out_b.front(), out_b.tangent_at(0.0)) <= 0.0)
						{
							continue; // on the other side of the node (the base radius is for this area)
						}
						if (d < need)
						{
							last_bad = s;
						}
					}
					if (last_bad >= 0.0)
					{
						wanted = std::max(wanted, last_bad + 0.5);
						a.trim = std::max(a.trim, std::min(s_max, last_bad + 0.5));
					}
				}
				a.clear = std::max(0.0, wanted - a.trim);
			}
		}
		junctions[n.id] = j;
	}

	// Trims and trimmed center lines
	for (auto &kv : roads)
	{
		net_road &r = kv.second;
		const road_data *rd = m.road(r.road_id);
		const net_junction *ja = junction(rd->node_a);
		const net_junction *jb = junction(rd->node_b);
		auto arm_trim = [&r](const net_junction *j, bool at_a)
		{
			if (!j)
			{
				return 0.0;
			}
			for (const net_arm &a : j->arms)
			{
				if (a.road_id == r.road_id && a.at_a == at_a)
				{
					return a.trim;
				}
			}
			return j->radius;
		};
		r.trim_a = arm_trim(ja, true);
		r.trim_b = arm_trim(jb, false);
		const double len = r.center.length();
		if (r.trim_a + r.trim_b > len * 0.9)
		{
			const double k = len * 0.9 / (r.trim_a + r.trim_b);
			r.trim_a *= k;
			r.trim_b *= k;
		}
		r.trimmed = r.center.sub(r.trim_a, len - r.trim_b);
		// Sidewalk center lines (for the pedestrians and the drawing)
		if (r.sidewalk_left)
		{
			r.sidewalk_left_line = r.trimmed.offset(r.edge_left + r.bike_width + r.sidewalk_width / 2.0);
		}
		if (r.sidewalk_right)
		{
			r.sidewalk_right_line = r.trimmed.offset(r.edge_right - r.bike_width - r.sidewalk_width / 2.0);
		}
	}

	// Junction surfaces and corners
	for (auto &kv : junctions)
	{
		net_junction &j = kv.second;
		if (j.arms.size() < 2)
		{
			continue;
		}
		std::vector<vec2> pts;
		pts.push_back(j.pos);
		for (const net_arm &a : j.arms)
		{
			const net_road &r = roads[a.road_id];
			// Lateral extent on the left and on the right of the arm direction
			const double ext_ccw = (a.at_a ? r.edge_left : -r.edge_right) + r.bike_width;
			const double ext_cw = (a.at_a ? -r.edge_right : r.edge_left) + r.bike_width;
			const double s = a.at_a ? r.trim_a : r.center.length() - r.trim_b;
			vec2 p;
			vec2 t;
			r.center.sample(s, &p, &t);
			const vec2 u = a.at_a ? t : -t;
			pts.push_back(p + u.left() * ext_ccw);
			pts.push_back(p + u.right() * ext_cw);
		}
		j.polygon = geom::convex_hull(pts);

		const size_t n = j.arms.size();
		j.corners.assign(n, j.pos);
		for (size_t k = 0; k < n; ++k)
		{
			const net_arm &a1 = j.arms[k];
			const net_arm &a2 = j.arms[(k + 1) % n];
			const net_road &r1 = roads[a1.road_id];
			const net_road &r2 = roads[a2.road_id];
			const double s1 = a1.at_a ? r1.trim_a : r1.center.length() - r1.trim_b;
			const double s2 = a2.at_a ? r2.trim_a : r2.center.length() - r2.trim_b;
			vec2 p1;
			vec2 t1;
			vec2 p2;
			vec2 t2;
			r1.center.sample(s1, &p1, &t1);
			r2.center.sample(s2, &p2, &t2);
			const vec2 u1 = a1.at_a ? t1 : -t1;
			const vec2 u2 = a2.at_a ? t2 : -t2;
			const double off1 = (a1.at_a ? r1.edge_left : -r1.edge_right) + r1.bike_width + r1.sidewalk_width / 2.0;
			const double off2 = (a2.at_a ? -r2.edge_right : r2.edge_left) + r2.bike_width + r2.sidewalk_width / 2.0;
			const vec2 c1 = p1 + u1.left() * off1;
			const vec2 c2 = p2 + u2.right() * off2;
			j.corners[k] = (c1 + c2) * 0.5;
		}
	}
}

namespace
{
	// Intersection of the segments a-b and c-d: *t is the position on a-b (0..1),
	// *sin_angle the sine of the angle between the two segments.
	bool segments_cross(const vec2 &a, const vec2 &b, const vec2 &c, const vec2 &d, double *t, double *sin_angle)
	{
		const vec2 r = b - a;
		const vec2 s = d - c;
		const double den = r.x * s.y - r.y * s.x;
		const double lr = r.length();
		const double ls = s.length();
		if (std::abs(den) < 1e-9 || lr < 1e-9 || ls < 1e-9)
		{
			return false;
		}
		const vec2 q = c - a;
		const double u = (q.x * s.y - q.y * s.x) / den;
		const double v = (q.x * r.y - q.y * r.x) / den;
		if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
		{
			return false;
		}
		*t = u;
		*sin_angle = std::abs(den) / (lr * ls);
		return true;
	}
}

void road_network::build_levels(const world_model &m)
{
	const double ramp_max = 25.0;
	for (auto &kv : roads)
	{
		net_road &r = kv.second;
		const road_data *rd = m.road(r.road_id);
		const double len = r.trimmed.length();
		r.level = rd ? rd->level : 0;
		r.level_s0 = 0.0;
		r.level_s1 = len;
		if (r.level == 0 || len <= 0.0)
		{
			continue;
		}
		// A ramp at an end where the junction has a road of another level
		// (or no other road)
		auto ramp_end = [&](int node)
		{
			bool any = false;
			for (int rid : m.roads_at_node(node))
			{
				const road_data *o = m.road(rid);
				if (rid == r.road_id || !o)
				{
					continue;
				}
				any = true;
				if (o->level != r.level)
				{
					return true;
				}
			}
			return !any;
		};
		const double ramp = std::min(ramp_max, len * 0.3);
		double s0 = ramp_end(rd->node_a) ? ramp : 0.0;
		double s1 = ramp_end(rd->node_b) ? len - ramp : len;
		// The raised or covered part contains each crossing with a road of
		// another level (and the width of that road)
		const std::vector<vec2> &pa = r.trimmed.points();
		const std::vector<double> &ca = r.trimmed.cumulative();
		for (const auto &kv2 : roads)
		{
			const net_road &o = kv2.second;
			const road_data *od = m.road(o.road_id);
			if (kv2.first == kv.first || !od || od->level == r.level || o.trimmed.empty())
			{
				continue;
			}
			const std::vector<vec2> &pb = o.trimmed.points();
			const double half = std::max(std::abs(o.outer_left), std::abs(o.outer_right)) + 1.5;
			for (size_t i = 0; i + 1 < pa.size(); ++i)
			{
				for (size_t k = 0; k + 1 < pb.size(); ++k)
				{
					double t = 0.0;
					double sin_angle = 1.0;
					if (!segments_cross(pa[i], pa[i + 1], pb[k], pb[k + 1], &t, &sin_angle))
					{
						continue;
					}
					const double s = ca[i] + t * (ca[i + 1] - ca[i]);
					const double reach = half / std::max(0.35, sin_angle);
					s0 = std::min(s0, s - reach);
					s1 = std::max(s1, s + reach);
				}
			}
		}
		r.level_s0 = std::max(0.0, std::min(s0, len));
		r.level_s1 = std::max(r.level_s0, std::min(s1, len));
	}
}

void road_network::build_lanes(const world_model &m)
{
	for (auto &kv : roads)
	{
		net_road &r = kv.second;
		const road_data *rd = m.road(r.road_id);
		const double lw = std::max(2.0, rd->lane_width);
		const int lf = std::max(0, rd->lanes_forward);
		const int lb = std::max(0, rd->lanes_backward);
		const bool two_way = lf > 0 && lb > 0;
		for (int dir : { 1, -1 })
		{
			const int count = dir > 0 ? lf : lb;
			if (count <= 0)
			{
				continue;
			}
			const polyline base = dir > 0 ? r.trimmed : r.trimmed.reversed();
			const double f_min = two_way ? -(r.median_width / 2.0 + count * lw) : -(count * lw) / 2.0;
			std::vector<int> &ids = m_lanes_by_dir[dir_key(r.road_id, dir)];
			for (int i = 0; i < count; ++i)
			{
				net_lane l;
				l.id = static_cast<int>(lanes.size());
				l.road_id = r.road_id;
				l.dir = dir;
				l.index = i;
				l.lanes_in_dir = count;
				l.from_node = dir > 0 ? rd->node_a : rd->node_b;
				l.to_node = dir > 0 ? rd->node_b : rd->node_a;
				l.path = base.offset(f_min + (i + 0.5) * lw);
				l.width = lw;
				l.base_limit = std::max(5.0, rd->speed_limit) / 3.6;
				const net_junction *jt = junction(l.to_node);
				l.dead_end = !jt || jt->arms.size() <= 1;
				l.stop_line = l.dead_end ? l.length() : std::max(0.0, l.length() - 0.5);
				if (jt && !l.dead_end)
				{
					// The end of a short road can still touch another arm: stop before it
					for (const net_arm &arm : jt->arms)
					{
						if (arm.road_id == r.road_id && arm.at_a == (dir < 0))
						{
							l.clear_end = arm.clear;
							break;
						}
					}
					l.stop_line = std::max(std::min(l.stop_line, l.length() - l.clear_end - 0.3), std::min(l.stop_line, l.length() * 0.5));
				}
				if (!rd->overtaking)
				{
					l.no_overtaking.push_back(std::make_pair(0.0, l.length() + 1.0));
				}
				ids.push_back(l.id);
				lanes.push_back(l);
			}
			for (size_t i = 0; i < ids.size(); ++i)
			{
				lanes[ids[i]].left = (i + 1 < ids.size()) ? ids[i + 1] : -1;
				lanes[ids[i]].right = (i > 0) ? ids[i - 1] : -1;
			}
			if (dir > 0)
			{
				r.lanes_fwd = ids;
			}
			else
			{
				r.lanes_bwd = ids;
			}
		}
	}
}

void road_network::build_connectors()
{
	for (auto &kv : junctions)
	{
		net_junction &j = kv.second;
		if (j.arms.size() < 2)
		{
			continue;
		}
		for (const net_arm &ain : j.arms)
		{
			// Lanes to this junction on the incoming arm
			const std::vector<int> &lin = lanes_of(ain.road_id, ain.at_a ? -1 : 1);
			if (lin.empty())
			{
				continue;
			}
			const net_lane &ref_in = lanes[lin.front()];
			const vec2 h_in = ref_in.path.tangent_at(ref_in.length());

			struct exit_info
			{
				const net_arm *arm;
				double delta;
				turn_kind kind;
			};
			std::vector<exit_info> exits;
			for (const net_arm &aout : j.arms)
			{
				if (aout.road_id == ain.road_id)
				{
					continue;
				}
				const std::vector<int> &lout = lanes_of(aout.road_id, aout.at_a ? 1 : -1);
				if (lout.empty())
				{
					continue;
				}
				const vec2 h_out = lanes[lout.front()].path.tangent_at(0.0);
				const double delta = wrap_angle(h_out.angle() - h_in.angle());
				if (std::abs(delta) > 160.0 * deg)
				{
					continue; // no U-turns in a junction
				}
				turn_kind kind = turn_kind::straight;
				if (delta < -30.0 * deg)
				{
					kind = turn_kind::right;
				}
				else if (delta > 30.0 * deg)
				{
					kind = turn_kind::left;
				}
				exits.push_back({ &aout, delta, kind });
			}
			if (exits.empty())
			{
				continue;
			}
			std::sort(exits.begin(), exits.end(), [](const exit_info &a, const exit_info &b) { return a.delta < b.delta; });
			const int n = static_cast<int>(lin.size());
			const int k_count = static_cast<int>(exits.size());
			bool has_straight = false;
			for (const exit_info &e : exits)
			{
				has_straight = has_straight || e.kind == turn_kind::straight;
			}

			for (const exit_info &e : exits)
			{
				// Lanes that serve this exit (right-hand traffic)
				int first = 0;
				int last = n - 1;
				if (k_count > 1)
				{
					if (e.kind == turn_kind::right)
					{
						first = 0;
						last = has_straight ? 0 : std::max(0, (n + 1) / 2 - 1);
					}
					else if (e.kind == turn_kind::left)
					{
						last = n - 1;
						first = has_straight ? n - 1 : std::min(n - 1, n / 2);
					}
				}
				const std::vector<int> &lout = lanes_of(e.arm->road_id, e.arm->at_a ? 1 : -1);
				const int m = static_cast<int>(lout.size());
				for (int i = first; i <= last; ++i)
				{
					int target = 0;
					if (e.kind == turn_kind::left)
					{
						target = std::max(0, (m - 1) - (last - i));
					}
					else if (e.kind == turn_kind::right)
					{
						target = std::min(m - 1, i - first);
					}
					else
					{
						target = std::min(m - 1, i);
					}
					const net_lane &from = lanes[lin[i]];
					const net_lane &to = lanes[lout[target]];
					net_connector c;
					c.id = static_cast<int>(connectors.size());
					c.node_id = j.node_id;
					c.from_lane = from.id;
					c.to_lane = to.id;
					c.from_road = from.road_id;
					c.to_road = to.road_id;
					c.turn = e.kind;
					const vec2 p0 = from.path.back();
					const vec2 t0 = from.path.tangent_at(from.length());
					const vec2 p3 = to.path.front();
					const vec2 t3 = to.path.tangent_at(0.0);
					const double d = distance(p0, p3);
					const double k = d * (e.kind == turn_kind::straight ? 0.33 : 0.45);
					const int segs = std::max(6, static_cast<int>(std::ceil(d / 0.8)));
					c.path = polyline(geom::bezier(p0, p0 + t0 * k, p3 - t3 * k, p3, segs));
					const double radius = c.path.min_radius();
					c.max_speed = std::min(std::min(from.base_limit, to.base_limit), std::max(3.0, std::sqrt(2.2 * radius)));
					lanes[from.id].out.push_back(c.id);
					j.connectors.push_back(c.id);
					connectors.push_back(c);
				}
			}
		}
	}
}

namespace
{
	// The body of a long vehicle on the movement c, as simulation::update_positions
	// places it: the front on the path, the rear "length" meters back on the
	// path, the box of the full length around the middle of the two points (on
	// a tight curve the box goes past the two points). Each item is the axis of
	// the box (front end, rear end), one for each "step" meters of the front.
	struct body_axis
	{
		vec2 a;
		vec2 b;
	};

	std::vector<body_axis> long_bodies(const net_connector &c, const net_lane &from, const net_lane &to, double length, double step)
	{
		// Path of the front: end of the incoming lane (a short lane: its line
		// continues to the back), connector, start of the exit lane
		std::vector<vec2> pts;
		for (double s = from.length() - length; s < from.length() - 0.01; s += step)
		{
			pts.push_back(from.path.point_at(s));
		}
		pts.insert(pts.end(), c.path.points().begin(), c.path.points().end());
		for (double s = step; s <= std::min(to.length(), length); s += step)
		{
			pts.push_back(to.path.point_at(s));
		}
		const polyline path(pts);
		std::vector<body_axis> out;
		for (double s = std::min(length, path.length()); s <= path.length() + 0.01; s += step)
		{
			const vec2 front = path.point_at(s);
			const vec2 rear = path.point_at(s - length);
			const vec2 d = front - rear;
			if (d.length_sq() < 1e-6)
			{
				continue;
			}
			const vec2 u = d.normalized();
			const vec2 mid = (front + rear) * 0.5;
			out.push_back({ mid + u * (length * 0.5), mid - u * (length * 0.5) });
		}
		return out;
	}

	struct bounds2
	{
		vec2 lo = vec2(1e18, 1e18);
		vec2 hi = vec2(-1e18, -1e18);
		void add(const vec2 &p)
		{
			lo = vec2(std::min(lo.x, p.x), std::min(lo.y, p.y));
			hi = vec2(std::max(hi.x, p.x), std::max(hi.y, p.y));
		}
		bool near(const bounds2 &o, double d) const
		{
			return lo.x - d <= o.hi.x && o.lo.x - d <= hi.x && lo.y - d <= o.hi.y && o.lo.y - d <= hi.y;
		}
	};
}

void road_network::build_conflicts()
{
	// The body of a 12 m vehicle cuts the curve by the sagitta of its length.
	auto cut = [](const net_connector &c)
	{
		const double r = c.path.min_radius();
		const double half = 6.0;
		return r > half ? std::min(3.0, r - std::sqrt(r * r - half * half)) : 3.0;
	};
	// Bodies of a 12 m vehicle on each movement and their bounds
	const double wide = 2.6; // half widths of two buses
	std::vector<std::vector<body_axis>> bodies(connectors.size());
	std::vector<bounds2> body_bounds(connectors.size());
	std::vector<bounds2> path_bounds(connectors.size());
	for (const net_connector &c : connectors)
	{
		bodies[c.id] = long_bodies(c, lanes[c.from_lane], lanes[c.to_lane], 12.0, 1.0);
		for (const body_axis &ax : bodies[c.id])
		{
			body_bounds[c.id].add(ax.a);
			body_bounds[c.id].add(ax.b);
		}
		for (const vec2 &p : c.path.points())
		{
			path_bounds[c.id].add(p);
		}
	}
	// A long vehicle on a touches a vehicle on the path of b
	auto body_touches = [&](const net_connector &a, const net_connector &b)
	{
		if (!body_bounds[a.id].near(path_bounds[b.id], wide))
		{
			return false;
		}
		const std::vector<vec2> &pts = b.path.points();
		for (const body_axis &ax : bodies[a.id])
		{
			for (size_t k = 0; k + 1 < pts.size(); ++k)
			{
				if (geom::segment_segment_distance(ax.a, ax.b, pts[k], pts[k + 1]) < wide)
				{
					return true;
				}
			}
		}
		return false;
	};
	for (auto &kv : junctions)
	{
		const std::vector<int> &ids = kv.second.connectors;
		for (size_t x = 0; x < ids.size(); ++x)
		{
			for (size_t y = x + 1; y < ids.size(); ++y)
			{
				net_connector &a = connectors[ids[x]];
				net_connector &b = connectors[ids[y]];
				if (a.from_lane == b.from_lane)
				{
					continue; // same lane: the vehicles follow each other
				}
				// Same exit lane, or paths nearer than the half widths of two wide
				// vehicles (a bus is 2.55 m wide).
				const double d = a.to_lane == b.to_lane ? 0.0 : geom::polyline_distance(a.path, b.path);
				if (d < 2.6)
				{
					a.conflicts.push_back(b.id);
					b.conflicts.push_back(a.id);
				}
				else if (d < 2.6 + std::max(cut(a), cut(b)) + 0.3 || body_touches(a, b) || body_touches(b, a))
				{
					a.conflicts_long.push_back(b.id);
					b.conflicts_long.push_back(a.id);
				}
			}
		}
	}
}

void road_network::build_crosswalks()
{
	for (auto &kv : junctions)
	{
		net_junction &j = kv.second;
		const bool wanted = j.arms.size() >= 3 || (j.arms.size() == 2 && j.light_id >= 0);
		if (!wanted)
		{
			continue;
		}
		for (size_t k = 0; k < j.arms.size(); ++k)
		{
			net_arm &arm = j.arms[k];
			net_road &r = roads[arm.road_id];
			if (!r.sidewalk_left && !r.sidewalk_right)
			{
				continue;
			}
			const double len = r.center.length();
			if (len - r.trim_a - r.trim_b < 2.0 * (crosswalk_gap + crosswalk_w) + 4.0)
			{
				continue; // the road is too short for a crosswalk
			}
			const double trim = arm.at_a ? r.trim_a : r.trim_b;
			const double d0 = trim + crosswalk_gap;
			const double d1 = d0 + crosswalk_w;
			const double s0 = arm.at_a ? d0 : len - d1;
			const double s1 = arm.at_a ? d1 : len - d0;
			const double sm = (s0 + s1) / 2.0;
			vec2 pm;
			vec2 t;
			r.center.sample(sm, &pm, &t);
			const vec2 nrm = t.left();
			const double out_l = r.edge_left + r.bike_width + r.sidewalk_width / 2.0;
			const double out_r = r.edge_right - r.bike_width - r.sidewalk_width / 2.0;

			net_crosswalk cw;
			cw.id = static_cast<int>(crosswalks.size());
			cw.node_id = j.node_id;
			cw.road_id = arm.road_id;
			cw.arm = static_cast<int>(k);
			const vec2 left_end = pm + nrm * out_l;
			const vec2 right_end = pm + nrm * out_r;
			// a: clockwise side of the arm (right of the direction from the junction)
			cw.a = arm.at_a ? right_end : left_end;
			cw.b = arm.at_a ? left_end : right_end;
			cw.length = distance(cw.a, cw.b);
			const vec2 q0 = r.center.point_at(s0);
			const vec2 q1 = r.center.point_at(s1);
			const double el = r.edge_left + r.bike_width;
			const double er = r.edge_right - r.bike_width;
			cw.polygon = { q0 + nrm * er, q1 + nrm * er, q1 + nrm * el, q0 + nrm * el };

			// Lanes that the crosswalk crosses
			for (int dir : { 1, -1 })
			{
				for (int lid : lanes_of(arm.road_id, dir))
				{
					net_lane &l = lanes[lid];
					if (l.to_node == j.node_id)
					{
						cw.lanes_in.push_back(lid);
						l.end_crosswalk = cw.id;
						l.stop_line = std::max(0.0, l.length() - std::max(crosswalk_gap + crosswalk_w + stop_gap, l.clear_end + 0.3));
					}
					else if (l.from_node == j.node_id)
					{
						cw.lanes_out.push_back(lid);
						l.start_crosswalk = cw.id;
						l.start_cw_s0 = crosswalk_gap;
						l.start_cw_s1 = crosswalk_gap + crosswalk_w;
					}
				}
			}
			arm.crosswalk = cw.id;
			crosswalks.push_back(cw);
		}
	}
}

void road_network::build_turn_clearance()
{
	// A turning bus cuts the inside of its curve with its body (long_bodies:
	// the same box as in the simulation). A vehicle that waits at the stop
	// line of another lane must be out of this area: the stop line moves
	// back, as the stop lines of real junctions. The turns of near junctions
	// count too (OSM junctions with nodes near each other).
	const double bus_length = 12.0;
	const double bus_half = 1.3;    // half width of a bus (2.55 m) and a margin
	const double car_half = 0.95;   // half width of a waiting vehicle (1.8 m) and a margin
	const double car_length = 4.8;
	const double step = 1.0;
	const double max_back = 8.0;
	const double cell = 16.0;
	struct chord
	{
		vec2 a;
		vec2 b;
		vec2 mid;
		int from_lane;
		int to_lane;
	};
	std::vector<chord> chords;
	std::unordered_map<qint64, std::vector<int>> grid;
	auto key = [cell](const vec2 &p, int dx, int dy)
	{
		const qint64 cx = static_cast<qint64>(std::floor(p.x / cell)) + dx;
		const qint64 cy = static_cast<qint64>(std::floor(p.y / cell)) + dy;
		return (cx << 32) ^ (cy & 0xFFFFFFFF);
	};
	for (const net_connector &c : connectors)
	{
		for (const body_axis &ax : long_bodies(c, lanes[c.from_lane], lanes[c.to_lane], bus_length, step))
		{
			const chord ch = { ax.a, ax.b, (ax.a + ax.b) * 0.5, c.from_lane, c.to_lane };
			grid[key(ch.mid, 0, 0)].push_back(static_cast<int>(chords.size()));
			chords.push_back(ch);
		}
	}
	int moved = 0;
	double total = 0.0;
	for (net_lane &l : lanes)
	{
		if (l.dead_end || l.out.empty())
		{
			continue;
		}
		auto blocked = [&](double s_front)
		{
			const vec2 p = l.path.point_at(s_front);
			const vec2 t = l.path.tangent_at(s_front);
			const vec2 n = t.left();
			const vec2 fl = p + n * car_half;
			const vec2 fr = p - n * car_half;
			const vec2 bl = fl - t * car_length;
			const vec2 br = fr - t * car_length;
			for (int dx = -1; dx <= 1; ++dx)
			{
				for (int dy = -1; dy <= 1; ++dy)
				{
					const auto it = grid.find(key(p, dx, dy));
					if (it == grid.end())
					{
						continue;
					}
					for (int ci : it->second)
					{
						const chord &ch = chords[ci];
						// The same lane: the vehicles follow each other
						if (ch.from_lane == l.id || ch.to_lane == l.id || distance(ch.mid, p) > bus_length * 0.5 + car_length + 1.5)
						{
							continue;
						}
						if (geom::segment_segment_distance(ch.a, ch.b, fl, fr) < bus_half
							|| geom::segment_segment_distance(ch.a, ch.b, fl, bl) < bus_half
							|| geom::segment_segment_distance(ch.a, ch.b, fr, br) < bus_half)
						{
							return true;
						}
					}
				}
			}
			return false;
		};
		// Not less than half of the lane, and space for one car if possible
		const double s_min = std::max(l.stop_line - max_back, std::min(l.stop_line, std::max(l.length() * 0.5, car_length + 3.0)));
		double s = l.stop_line;
		while (s > s_min && blocked(s))
		{
			s = std::max(s_min, s - 0.5);
		}
		if (s < l.stop_line)
		{
			total += l.stop_line - s;
			++moved;
			l.stop_line = s;
		}
	}
	if (moved > 0)
	{
		TE_DEBUG("NETWORK", QStringLiteral("Stop lines moved back for the turns of buses: %1 lanes, mean %2 m")
			.arg(moved).arg(total / moved, 0, 'f', 1));
	}
}

void road_network::apply_signs(const world_model &m)
{
	const sign_catalog &cat = sign_catalog::instance();
	for (const auto &kv : m.signs)
	{
		const sign_data &s = kv.second;
		const sign_def *def = cat.find(s.key);
		if (!def || def->effect == sign_effect::none)
		{
			continue;
		}
		for (int lid : lanes_of(s.road_id, s.dir))
		{
			net_lane &l = lanes[lid];
			const double ls = std::max(0.0, std::min(l.length(), lane_s_from_road_s(l, s.s)));
			switch (def->effect)
			{
				case sign_effect::speed_limit:
				{
					const int v = s.value > 0 ? s.value : def->default_value;
					l.limits.push_back({ ls, std::max(5, v) / 3.6 });
					break;
				}
				case sign_effect::end_speed_limit:
				case sign_effect::end_all:
					l.limits.push_back({ ls, l.base_limit });
					for (auto &z : l.no_overtaking)
					{
						if (def->effect == sign_effect::end_all && z.first <= ls && z.second > ls)
						{
							z.second = ls;
						}
					}
					break;
				case sign_effect::stop:
					l.stop_sign = true;
					l.rank = 0;
					l.sign_id = s.id;
					break;
				case sign_effect::yield:
					l.yield_sign = true;
					l.rank = 0;
					if (!l.stop_sign)
					{
						l.sign_id = s.id;
					}
					break;
				case sign_effect::priority_road:
					if (l.rank > 0)
					{
						l.rank = 2;
					}
					break;
				case sign_effect::end_priority_road:
					if (l.rank == 2)
					{
						l.rank = 1;
					}
					break;
				case sign_effect::no_entry:
				case sign_effect::road_closed:
					l.closed = true;
					break;
				case sign_effect::no_overtaking:
					l.no_overtaking.push_back(std::make_pair(ls, l.length() + 1.0));
					break;
				case sign_effect::end_no_overtaking:
					for (auto &z : l.no_overtaking)
					{
						if (z.first <= ls && z.second > ls)
						{
							z.second = ls;
						}
					}
					break;
				case sign_effect::no_left:
				case sign_effect::no_right:
				case sign_effect::no_uturn:
				case sign_effect::only_straight:
				case sign_effect::only_left:
				case sign_effect::only_right:
				case sign_effect::straight_or_right:
				case sign_effect::straight_or_left:
				case sign_effect::left_or_right:
					for (int cid : l.out)
					{
						net_connector &c = connectors[cid];
						const sign_effect e = def->effect;
						if ((e == sign_effect::no_left && c.turn == turn_kind::left)
							|| (e == sign_effect::no_right && c.turn == turn_kind::right)
							|| (e == sign_effect::no_uturn && c.turn == turn_kind::uturn)
							|| (e == sign_effect::only_straight && c.turn != turn_kind::straight)
							|| (e == sign_effect::only_left && c.turn != turn_kind::left)
							|| (e == sign_effect::only_right && c.turn != turn_kind::right)
							|| (e == sign_effect::straight_or_right && c.turn != turn_kind::straight && c.turn != turn_kind::right)
							|| (e == sign_effect::straight_or_left && c.turn != turn_kind::straight && c.turn != turn_kind::left)
							|| (e == sign_effect::left_or_right && c.turn != turn_kind::left && c.turn != turn_kind::right))
						{
							c.allowed = false;
						}
					}
					break;
				default:
					break;
			}
		}
	}
	for (net_lane &l : lanes)
	{
		std::sort(l.limits.begin(), l.limits.end(), [](const net_zone &a, const net_zone &b) { return a.s < b.s; });
	}
}

void road_network::apply_objects(const world_model &m)
{
	for (const auto &kv : m.objects)
	{
		const object_data &o = kv.second;
		const net_road *r = road(o.road_id);
		if (!r)
		{
			continue;
		}
		for (int dir : { 1, -1 })
		{
			if (o.dir != 0 && o.dir != dir
				&& o.kind != object_kind::level_crossing)
			{
				continue;
			}
			const std::vector<int> &ids = lanes_of(o.road_id, dir);
			for (int lid : ids)
			{
				net_lane &l = lanes[lid];
				const double ls = std::max(0.0, std::min(l.length(), lane_s_from_road_s(l, o.s)));
				switch (o.kind)
				{
					case object_kind::cone:
					case object_kind::barrier:
					{
						const int lane_index = std::min(o.lane, static_cast<int>(ids.size()) - 1);
						if (o.lane >= 0 && l.index != lane_index)
						{
							break;
						}
						l.blocks.push_back({ ls - o.length / 2.0, ls + o.length / 2.0, o.id });
						break;
					}
					case object_kind::road_closure:
						l.closed = true;
						break;
					case object_kind::level_crossing:
						l.level_crossings.push_back({ ls, o.id });
						break;
					case object_kind::bus_stop:
						if (l.index == 0)
						{
							l.bus_stops.push_back({ ls, o.id });
						}
						break;
				}
			}
		}
	}
	for (net_lane &l : lanes)
	{
		std::sort(l.bus_stops.begin(), l.bus_stops.end(), [](const net_stop_point &a, const net_stop_point &b) { return a.s < b.s; });
		std::sort(l.level_crossings.begin(), l.level_crossings.end(), [](const net_stop_point &a, const net_stop_point &b) { return a.s < b.s; });
	}
}

void road_network::build_index()
{
	m_index->tree.clear();
	std::vector<rvalue> values;
	for (const auto &kv : roads)
	{
		const net_road &r = kv.second;
		const double w = std::max(std::abs(r.outer_left), std::abs(r.outer_right)) + 1.0;
		const std::vector<vec2> &pts = r.center.points();
		for (size_t i = 0; i + 1 < pts.size(); ++i)
		{
			const double x0 = std::min(pts[i].x, pts[i + 1].x) - w;
			const double y0 = std::min(pts[i].y, pts[i + 1].y) - w;
			const double x1 = std::max(pts[i].x, pts[i + 1].x) + w;
			const double y1 = std::max(pts[i].y, pts[i + 1].y) + w;
			values.push_back(std::make_pair(bbox(bpoint(x0, y0), bpoint(x1, y1)), r.road_id));
		}
	}
	// Packing (bulk load) gives a better tree than single inserts.
	m_index->tree = bgi::rtree<rvalue, bgi::quadratic<16>>(values.begin(), values.end());
}

int road_network::road_at(const vec2 &p, double tolerance, double *s, double *lateral) const
{
	std::vector<rvalue> hits;
	const bbox q(bpoint(p.x - tolerance, p.y - tolerance), bpoint(p.x + tolerance, p.y + tolerance));
	m_index->tree.query(bgi::intersects(q), std::back_inserter(hits));
	std::set<int> ids;
	for (const rvalue &h : hits)
	{
		ids.insert(h.second);
	}
	int best = -1;
	double best_d = 1e18;
	for (int id : ids)
	{
		const net_road &r = roads.at(id);
		double d = 0.0;
		const double sp = r.center.project(p, &d);
		vec2 cp;
		vec2 t;
		r.center.sample(sp, &cp, &t);
		const double side = cross(t, p - cp);
		const double lat = side >= 0.0 ? d : -d;
		const double limit = (lat >= 0.0 ? r.outer_left : -r.outer_right) + tolerance;
		if (d <= limit && d < best_d)
		{
			best_d = d;
			best = id;
			if (s)
			{
				*s = sp;
			}
			if (lateral)
			{
				*lateral = lat;
			}
		}
	}
	return best;
}

int road_network::lane_from_lateral(int road_id, double lateral) const
{
	const net_road *r = road(road_id);
	if (!r)
	{
		return -1;
	}
	int best = -1;
	double best_d = 1e18;
	for (int dir : { 1, -1 })
	{
		for (int lid : lanes_of(road_id, dir))
		{
			const net_lane &l = lanes[lid];
			// Lateral position of the lane center in the a -> b frame
			const polyline &base = r->trimmed;
			const vec2 p0 = l.path.point_at(l.length() / 2.0);
			double d = 0.0;
			const double sp = base.project(p0, &d);
			vec2 cp;
			vec2 t;
			base.sample(sp, &cp, &t);
			const double lat = cross(t, p0 - cp) >= 0.0 ? d : -d;
			const double dd = std::abs(lat - lateral);
			if (dd < best_d)
			{
				best_d = dd;
				best = lid;
			}
		}
	}
	return best;
}
